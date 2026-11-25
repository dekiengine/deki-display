#include "DsiDisplay.h"

#include "DekiGPIO.h"  // from deki-gpio

#include <deki/LogSystem.h>
#include <deki/providers/Memory.h>

#include <algorithm>
#include <cstring>

namespace DekiDisplay
{

DsiDisplay::DsiDisplay(IDsiPanel& panel, int32_t width, int32_t height, uint32_t refreshPeriodUs)
    : m_Panel(panel), m_Width(width), m_Height(height), m_RefreshUs(refreshPeriodUs)
{
}

DsiDisplay::~DsiDisplay()
{
    Shutdown();
}

bool DsiDisplay::Start(const Options& options)
{
    m_Options = options;
    const int32_t buffers = m_Panel.GetFrameBufferCount();
    if (buffers < 1 || m_Width <= 0 || m_Height <= 0)
    {
        DEKI_LOG_ERROR("DsiDisplay: the panel has no frame buffer to draw into");
        return false;
    }

    // The backlight off while the buffers are cleared: PWM where the
    // platform has it and the board asks for it, the pin switched otherwise.
    m_Pwm = options.backlightPin >= 0 && options.backlightPwmHz > 0 &&
            m_Panel.StartBacklightPwm(options.backlightPin, options.backlightPwmHz, options.backlightInvert);
    DekiGpio::IDekiGPIO* gpio = DekiGpio::DekiGPIO::GetCurrent();
    if (!m_Pwm && gpio && options.backlightPin >= 0)
    {
        gpio->SetOutput(options.backlightPin, options.backlightInvert);
    }

    // Whatever the memory held at power-on: black before the light.
    const size_t bytes = static_cast<size_t>(m_Width) * m_Height * 2;
    for (int32_t i = 0; i < buffers; ++i)
    {
        if (uint8_t* fb = m_Panel.GetFrameBuffer(i))
        {
            std::memset(fb, 0, bytes);
        }
    }
    m_Shown = 0;
    m_Lent = -1;
    m_Panel.Show(0);
    // Until a refresh ends, the panel may still be sending what it held.
    if (!m_Panel.WaitRefresh(m_Options.refreshTimeoutMs))
    {
        SayLate();
    }

    m_Started = true;
    m_StripsOn = options.strips && buffers > 1;
    if (m_StripsOn)
    {
        m_Drawn.assign(static_cast<size_t>(m_Height), Span{});
        for (std::vector<Span>& stale : m_Stale)
        {
            stale.assign(static_cast<size_t>(m_Height), Span{ 0, m_Width });  // nothing known of any buffer yet
        }
        DekiRendering::RegisterScanOrderedDisplay(this, this);
    }
    SetBacklight(true);
    DEKI_LOG_INFO("DsiDisplay: %dx%d RGB565, %d frame buffer%s, a refresh every %u us, backlight %s",
                  static_cast<int>(m_Width), static_cast<int>(m_Height), static_cast<int>(buffers),
                  buffers == 1 ? "" : "s", static_cast<unsigned>(m_RefreshUs),
                  m_Pwm ? "PWM" : (options.backlightPin >= 0 ? "on/off" : "none"));
    return true;
}

bool DsiDisplay::Initialize(int32_t, int32_t)
{
    // The panel is started by DsiPanelComponent, through Start().
    DEKI_LOG_ERROR("DsiDisplay::Initialize() called directly - use the DsiPanelComponent instead");
    return false;
}

void DsiDisplay::Shutdown()
{
    if (!m_Started)
    {
        return;
    }
    SetBacklight(false);
    if (m_StripsOn)
    {
        m_Panel.WaitRowsCopy();
        DekiRendering::RegisterScanOrderedDisplay(this, nullptr);
        FreeStripBuffers();
        m_StripsOn = false;
    }
    m_Started = false;
}

void DsiDisplay::FreeStripBuffers()
{
    for (uint8_t*& b : m_StripBuffers)
    {
        if (b)
        {
            Deki::Memory::Free(b);
            b = nullptr;
        }
    }
    m_StripBytes = 0;
}

uint8_t* DsiDisplay::GetStripBuffer(size_t bytes)
{
    if (!m_StripsOn)
    {
        return nullptr;
    }
    if (bytes > m_StripBytes)
    {
        // Not tried again at a size they could not be had at.
        if (m_StripsFailed != 0 && bytes >= m_StripsFailed)
        {
            return nullptr;
        }
        // Both, at the size asked, in internal RAM a DMA reads.
        m_Panel.WaitRowsCopy();
        FreeStripBuffers();
        m_StripBuffers[0] = static_cast<uint8_t*>(Deki::Memory::AllocateDma(bytes, Deki::Memory::Internal));
        m_StripBuffers[1] = static_cast<uint8_t*>(Deki::Memory::AllocateDma(bytes, Deki::Memory::Internal));
        if (!m_StripBuffers[0] || !m_StripBuffers[1])
        {
            FreeStripBuffers();
            m_StripsFailed = bytes;
            DEKI_LOG_WARNING("DsiDisplay: no internal memory for two strips of %u bytes; strips are drawn in the "
                             "renderer's own buffer and copied on the CPU. Lower strips (the Rendering settings' "
                             "strip height) fit",
                             static_cast<unsigned>(bytes));
            return nullptr;
        }
        m_StripBytes = bytes;
    }
    // The other one: its copy into the frame was waited for before this
    // one's started (one copy at a time), so it is free.
    uint8_t* b = m_StripBuffers[m_NextStrip];
    m_NextStrip ^= 1;
    return b;
}

void DsiDisplay::CopyRowsNow(int32_t index, int32_t y, int32_t rows, const uint8_t* src, int32_t stride)
{
    m_Panel.WaitRowsCopy();
    if (m_Panel.StartRowsCopy(index, y, rows, src, stride))
    {
        m_Panel.WaitRowsCopy();
        return;
    }
    uint8_t* fb = m_Panel.GetFrameBuffer(index);
    const size_t rowBytes = static_cast<size_t>(m_Width) * 2;
    for (int32_t r = 0; r < rows; ++r)
    {
        std::memcpy(fb + static_cast<size_t>(y + r) * rowBytes, src + static_cast<size_t>(r) * stride * 2, rowBytes);
    }
}

void DsiDisplay::KeepShownRows(int32_t index, int32_t y, int32_t rows)
{
    if (index == m_Shown || rows <= 0)
    {
        return;
    }
    m_Panel.WaitRowsCopy();
    if (!m_Panel.CopyRowsBetween(m_Shown, index, y, rows))
    {
        const size_t rowBytes = static_cast<size_t>(m_Width) * 2;
        std::memcpy(m_Panel.GetFrameBuffer(index) + static_cast<size_t>(y) * rowBytes,
                    m_Panel.GetFrameBuffer(m_Shown) + static_cast<size_t>(y) * rowBytes,
                    static_cast<size_t>(rows) * rowBytes);
    }
}

void DsiDisplay::PresentStrip(const DekiRendering::ScanStrip& s)
{
    if (!m_Started || !m_StripsOn || !s.pixels || s.format != Deki::ColorFormat::RGB565)
    {
        return;
    }
    if (m_Assembling < 0 || s.firstOfFrame)
    {
        m_Assembling = FreeBuffer();
        std::fill(m_Drawn.begin(), m_Drawn.end(), Span{});
    }
    const int32_t top = std::max<int32_t>(0, s.strip.top);
    const int32_t bottom = std::min<int32_t>(m_Height, s.strip.bottom);
    const Deki::Rect& c = s.changed;
    if (bottom <= top || c.right <= c.left || c.bottom <= c.top)
    {
        return;  // nothing changed: its rows come from the frame shown before
    }
    const bool lent = s.pixels == m_StripBuffers[0] || s.pixels == m_StripBuffers[1];
    const int32_t y0 = std::max(top, c.top);
    const int32_t y1 = std::min(bottom, c.bottom);
    if (c.left <= 0 && c.right >= m_Width)
    {
        // Whole rows (the renderer sends a strip's whole width): those that
        // changed, in the background from a lent buffer while the renderer
        // draws the next strip in the other one; a buffer of the renderer's
        // own is reused at once, so copied now. The strip's other rows come
        // from the frame shown before, at Present.
        const uint8_t* from = s.pixels + static_cast<size_t>(y0 - top) * s.stride * 2;
        m_Panel.WaitRowsCopy();
        if (!m_Panel.StartRowsCopy(m_Assembling, y0, y1 - y0, from, s.stride))
        {
            CopyRowsNow(m_Assembling, y0, y1 - y0, from, s.stride);
        }
        else if (!lent)
        {
            m_Panel.WaitRowsCopy();
        }
        std::fill(m_Drawn.begin() + y0, m_Drawn.begin() + y1, Span{ 0, m_Width });
        return;
    }
    {
        // Part of it: what changed, on the CPU. The rest of its rows come from
        // the frame shown at EndStrips, where this buffer does not hold them.
        m_Panel.WaitRowsCopy();
        uint8_t* fb = m_Panel.GetFrameBuffer(m_Assembling);
        const int32_t x0 = std::max<int32_t>(0, c.left);
        const int32_t x1 = std::min<int32_t>(m_Width, c.right);
        for (int32_t y = y0; y < y1; ++y)
        {
            std::memcpy(fb + (static_cast<size_t>(y) * m_Width + x0) * 2,
                        s.pixels + (static_cast<size_t>(y - top) * s.stride + x0) * 2,
                        static_cast<size_t>(x1 - x0) * 2);
        }
        // A row may get several parts of a frame, apart: its span then runs
        // across both, so what lies between, not drawn, is brought from the
        // frame shown now where this buffer holds it stale.
        const std::vector<Span>& stale = m_Stale[Slot(m_Assembling)];
        const uint8_t* shown = m_Assembling != m_Shown ? m_Panel.GetFrameBuffer(m_Shown) : nullptr;
        for (int32_t y = y0; y < y1; ++y)
        {
            Span& d = m_Drawn[static_cast<size_t>(y)];
            if (d.Empty())
            {
                d = Span{ x0, x1 };
                continue;
            }
            // Overlapping or touching, there is no gap.
            const Span gap = d.right <= x0 ? Span{ d.right, x0 } : (x1 <= d.left ? Span{ x1, d.left } : Span{});
            const Span old = stale[static_cast<size_t>(y)];
            const int32_t l = std::max(gap.left, old.left), r = std::min(gap.right, old.right);
            if (shown && r > l)
            {
                const size_t at = (static_cast<size_t>(y) * m_Width + l) * 2;
                std::memcpy(fb + at, shown + at, static_cast<size_t>(r - l) * 2);
            }
            d = Span{ std::min(d.left, x0), std::max(d.right, x1) };
        }
    }
}

void DsiDisplay::SayLate()
{
    if (!m_SaidLate)
    {
        m_SaidLate = true;
        DEKI_LOG_WARNING("DsiDisplay: the panel did not finish a refresh within %u ms; frames are drawn without "
                         "waiting for it and may tear",
                         static_cast<unsigned>(m_Options.refreshTimeoutMs));
    }
}

int32_t DsiDisplay::FreeBuffer()
{
    const int32_t buffers = m_Panel.GetFrameBufferCount();
    if (buffers <= 1)
    {
        return 0;  // drawn while shown
    }
    // The first after the one shown that the panel is not reading; with
    // two buffers there is none until the refresh sending the other ends.
    for (int attempt = 0; attempt < 3; ++attempt)
    {
        const uint32_t busy = m_Panel.GetBusyMask();
        for (int32_t step = 1; step < buffers; ++step)
        {
            const int32_t i = (m_Shown + step) % buffers;
            if ((busy & (1u << i)) == 0)
            {
                return i;
            }
        }
        if (!m_Panel.WaitRefresh(m_Options.refreshTimeoutMs))
        {
            break;
        }
    }
    SayLate();
    return (m_Shown + 1) % buffers;
}

uint8_t* DsiDisplay::GetRenderBuffer(int32_t* width, int32_t* height)
{
    if (!m_Started)
    {
        return nullptr;
    }
    if (width)
    {
        *width = m_Width;
    }
    if (height)
    {
        *height = m_Height;
    }
    // One the panel is not reading, the same until the next Present.
    if (m_Lent < 0)
    {
        m_Lent = FreeBuffer();
    }
    return m_Panel.GetFrameBuffer(m_Lent);
}

void DsiDisplay::Present(const uint8_t* framebuffer, int width, int height, Deki::ColorFormat format)
{
    if (!m_Started || !framebuffer)
    {
        return;
    }
    EndStrips();  // a frame begun in strips is shown first, whole
    if (width != m_Width || height != m_Height || format != Deki::ColorFormat::RGB565)
    {
        if (!m_SaidFormat)
        {
            m_SaidFormat = true;
            DEKI_LOG_ERROR("DsiDisplay: a %dx%d frame in format %d cannot be shown on the %dx%d RGB565 panel", width,
                           height, static_cast<int>(format), static_cast<int>(m_Width), static_cast<int>(m_Height));
        }
        return;
    }

    int32_t index = -1;
    for (int32_t i = 0; i < m_Panel.GetFrameBufferCount(); ++i)
    {
        if (m_Panel.GetFrameBuffer(i) == framebuffer)
        {
            index = i;
            break;
        }
    }
    if (index < 0)
    {
        // Drawn in a buffer of the game's own (it did not take the one lent
        // to it): copied into a free one.
        index = m_Lent >= 0 ? m_Lent : FreeBuffer();
        const size_t bytes = static_cast<size_t>(m_Width) * m_Height * 2;
        if (!m_SaidCopying)
        {
            m_SaidCopying = true;
            DEKI_LOG_INFO("DsiDisplay: the game draws into a buffer of its own, so each frame is copied (%u bytes)",
                          static_cast<unsigned>(bytes));
        }
        std::memcpy(m_Panel.GetFrameBuffer(index), framebuffer, bytes);
    }

    m_Panel.Show(index);
    MarkShown(index, nullptr);
    m_Lent = -1;
}

void DsiDisplay::EndStrips()
{
    if (!m_Started || m_Assembling < 0)
    {
        return;
    }
    m_Panel.WaitRowsCopy();
    // What no strip brought: from the frame shown, but only what this buffer
    // does not already hold as shown.
    KeepStaleParts(m_Assembling);
    m_Panel.WaitRowsCopy();
    m_Panel.Show(m_Assembling);
    MarkShown(m_Assembling, m_Drawn.data());
    m_Assembling = -1;
    m_Lent = -1;
}

void DsiDisplay::KeepStaleParts(int32_t index)
{
    if (index == m_Shown)
    {
        return;
    }
    const std::vector<Span>& stale = m_Stale[Slot(index)];
    uint8_t* const to = m_Panel.GetFrameBuffer(index);
    const uint8_t* const from = m_Panel.GetFrameBuffer(m_Shown);
    const size_t rowBytes = static_cast<size_t>(m_Width) * 2;
    auto piece = [&](int32_t y, int32_t left, int32_t right)
    {
        if (right > left)
        {
            const size_t at = static_cast<size_t>(y) * rowBytes + static_cast<size_t>(left) * 2;
            std::memcpy(to + at, from + at, static_cast<size_t>(right - left) * 2);
        }
    };
    int32_t run = -1;  // the first of the whole rows waiting to be copied together
    for (int32_t y = 0; y <= m_Height; ++y)
    {
        const Span s = y < m_Height ? stale[static_cast<size_t>(y)] : Span{};
        const Span d = y < m_Height ? m_Drawn[static_cast<size_t>(y)] : Span{};
        const bool wholeRow = y < m_Height && d.Empty() && s.left <= 0 && s.right >= m_Width;
        if (wholeRow)
        {
            run = run < 0 ? y : run;
            continue;
        }
        if (run >= 0)
        {
            KeepShownRows(index, run, y - run);
            m_Panel.WaitRowsCopy();
            run = -1;
        }
        if (y == m_Height || s.Empty())
        {
            continue;
        }
        if (d.Empty())
        {
            piece(y, s.left, s.right);
        }
        else
        {
            piece(y, s.left, std::min(s.right, d.left));
            piece(y, std::max(s.left, d.right), s.right);
        }
    }
}

void DsiDisplay::MarkShown(int32_t index, const Span* drawn)
{
    // The buffer shown now is the frame; every other one now may differ from
    // it where it did before and where this frame drew (everywhere, for a
    // whole frame).
    m_Shown = index;
    if (m_Stale[0].size() != static_cast<size_t>(m_Height))
    {
        return;  // not drawing in strips: nothing is kept
    }
    const int32_t buffers = std::min(m_Panel.GetFrameBufferCount(), kMaxBuffers);
    for (int32_t b = 0; b < buffers; ++b)
    {
        std::vector<Span>& stale = m_Stale[b];
        if (b == Slot(index))
        {
            std::fill(stale.begin(), stale.end(), Span{});
            continue;
        }
        for (int32_t y = 0; y < m_Height; ++y)
        {
            Span& s = stale[static_cast<size_t>(y)];
            const Span d = drawn ? drawn[y] : Span{ 0, m_Width };
            if (d.Empty())
            {
                continue;
            }
            s = s.Empty() ? d : Span{ std::min(s.left, d.left), std::max(s.right, d.right) };
        }
    }
}

void DsiDisplay::GetDisplaySize(int32_t* width, int32_t* height) const
{
    if (width)
    {
        *width = m_Width;
    }
    if (height)
    {
        *height = m_Height;
    }
}

void DsiDisplay::SetBacklight(bool on)
{
    m_LightOn = on;
    if (m_Pwm)
    {
        m_Panel.SetBacklightLevel(on ? m_Options.brightness : 0.0f);
        return;
    }
    DekiGpio::IDekiGPIO* gpio = DekiGpio::DekiGPIO::GetCurrent();
    if (gpio && m_Options.backlightPin >= 0)
    {
        gpio->Write(m_Options.backlightPin, on != m_Options.backlightInvert);
    }
}

void DsiDisplay::SetBrightness(float brightness)
{
    m_Options.brightness = brightness < 0.0f ? 0.0f : (brightness > 1.0f ? 1.0f : brightness);
    if (m_Pwm && m_LightOn)
    {
        m_Panel.SetBacklightLevel(m_Options.brightness);
    }
}

uint32_t DsiDisplay::GetRefreshPeriodUs() const
{
    return m_Panel.GetFrameBufferCount() > 1 ? m_RefreshUs : 0u;
}

bool DsiDisplay::GetFramesShown(uint64_t* frames, uint64_t* lastShownUs) const
{
    return m_Panel.GetFramesShown(frames, lastShownUs);
}

}  // namespace DekiDisplay
