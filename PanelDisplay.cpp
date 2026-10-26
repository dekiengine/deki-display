#include "PanelDisplay.h"

#include "DekiGPIO.h"  // from deki-gpio
#include <deki/LogSystem.h>
#include <deki/Time.h>
#include <deki/providers/Memory.h>

#include <algorithm>
#include <cstring>

namespace DekiDisplay
{

namespace
{

const PanelDisplay::WorkerBackend* s_Worker = nullptr;

inline uint16_t SwapBytes16(uint16_t v)
{
    return static_cast<uint16_t>((v >> 8) | (v << 8));
}

// Swaps the bytes of `pixels` RGB565 pixels in place, two at a time.
void SwapPixels(uint16_t* p, size_t pixels)
{
    if (pixels == 0)
    {
        return;
    }
    if (reinterpret_cast<uintptr_t>(p) & 2)
    {
        *p = SwapBytes16(*p);
        ++p;
        --pixels;
    }
    auto* words = reinterpret_cast<uint32_t*>(p);
    for (size_t i = 0; i < pixels / 2; ++i)
    {
        const uint32_t v = words[i];
        words[i] = ((v >> 8) & 0x00FF00FF) | ((v & 0x00FF00FF) << 8);
    }
    if (pixels & 1)
    {
        p[pixels - 1] = SwapBytes16(p[pixels - 1]);
    }
}

// An overlay pixel (ARGB8888, alpha not 0) over an RGB565 one, both values in
// the CPU's byte order.
inline uint16_t BlendOverlayPixel(uint16_t bg, uint32_t argb)
{
    if ((argb & 0xFF000000) == 0xFF000000)
    {
        return static_cast<uint16_t>(((argb >> 8) & 0xF800) | ((argb >> 5) & 0x07E0) | ((argb >> 3) & 0x001F));
    }
    const uint8_t alpha = argb >> 24;
    const uint8_t r = (argb >> 16) & 0xFF;
    const uint8_t g = (argb >> 8) & 0xFF;
    const uint8_t b = argb & 0xFF;

    const uint8_t bgR = (bg >> 8) & 0xF8;
    const uint8_t bgG = (bg >> 3) & 0xFC;
    const uint8_t bgB = (bg << 3) & 0xF8;

    const uint8_t invAlpha = 255 - alpha;
    const uint8_t outR = (r * alpha + bgR * invAlpha + 128) >> 8;
    const uint8_t outG = (g * alpha + bgG * invAlpha + 128) >> 8;
    const uint8_t outB = (b * alpha + bgB * invAlpha + 128) >> 8;

    return static_cast<uint16_t>(((outR & 0xF8) << 8) | ((outG & 0xFC) << 3) | (outB >> 3));
}

// Finds where a row of the overlay is not fully transparent.
void MeasureOverlayRow(const uint32_t* row, int width, uint16_t& left, uint16_t& right)
{
    int l = 0;
    while (l < width && (row[l] & 0xFF000000) == 0)
    {
        ++l;
    }
    int r = width;
    while (r > l && (row[r - 1] & 0xFF000000) == 0)
    {
        --r;
    }
    left = static_cast<uint16_t>(l);
    right = static_cast<uint16_t>(r);
}

}  // namespace

PanelDisplay::PanelDisplay(PanelController& controller, IPanelIO& io)
    : m_Controller(controller),
      m_IO(io)
{
}

PanelDisplay::~PanelDisplay()
{
    Shutdown();
}

bool PanelDisplay::Start(const Options& options)
{
    if (m_Initialized)
    {
        return true;
    }
    m_Options = options;
    m_Width = m_Controller.GetWidth();
    m_Height = m_Controller.GetHeight();
    if (m_Width <= 0 || m_Height <= 0)
    {
        DEKI_LOG_ERROR("PanelDisplay: the panel has no size (%dx%d)", (int)m_Width, (int)m_Height);
        return false;
    }
    m_WireBigEndian = m_IO.PixelsBigEndian() != options.swapBytes;
    m_SwapRGB565 = m_WireBigEndian != Deki::kRGB565BigEndian;
    m_PanelWidth = m_Width;
    m_PanelHeight = m_Height;
    // May turn the controller to its own orientation, so before anything is
    // sized by it or sent.
    StartTearFree(options.tePin, options.scanlineSync);

    // A band is at most one transfer, and a whole number of rows.
    const size_t rowBytes = static_cast<size_t>(m_PanelWidth) * sizeof(uint16_t);
    const size_t maxRows = std::max<size_t>(1, m_IO.GetMaxChunkBytes() / rowBytes);
    m_BandRows = static_cast<int32_t>(std::clamp<size_t>(options.bandRows > 0 ? options.bandRows : 16, 1, maxRows));
    if (!EnsureBands())
    {
        return false;
    }

    if (options.usePsram || options.doubleBuffer)
    {
        // The engine draws into these, and they are sent by DMA like the
        // bands, so they must be DMA-capable wherever they live.
        const size_t bytes = static_cast<size_t>(m_Width) * m_Height * sizeof(uint16_t);
        const Deki::Memory::Region region = options.usePsram ? Deki::Memory::External : Deki::Memory::Internal;
        for (int i = 0; i < (options.doubleBuffer ? 2 : 1); ++i)
        {
            m_RenderBuffers[i] = static_cast<uint16_t*>(Deki::Memory::AllocateDma(bytes, region));
            if (!m_RenderBuffers[i])
            {
                DEKI_LOG_WARNING("PanelDisplay: no room for render buffer %d (%zu bytes, %s); %s", i, bytes,
                                 options.usePsram ? "external" : "internal",
                                 i == 0 ? "the engine draws into its own" : "one buffer only");
                break;
            }
            memset(m_RenderBuffers[i], 0, bytes);
        }
    }

    m_Initialized = true;
    DekiGpio::IDekiGPIO* gpio = DekiGpio::DekiGPIO::GetCurrent();
    if (gpio && options.backlightPin >= 0)
    {
        gpio->SetOutput(options.backlightPin, options.backlightInvert);  // off while the panel is cleared
    }
    // The panel's memory holds noise at power-on: black before the light.
    Clear();
    SetBacklight(true);
    // The bands come back on the first present that needs them. A frame
    // drawn in strips never does: its strips are sent from their own copies,
    // and 20 KB of internal RAM is the scene's.
    FreeBands();
    DEKI_LOG_INFO("PanelDisplay: %dx%d %s, bands of %d rows, %s on the wire", (int)m_Width, (int)m_Height,
                  m_Controller.GetInfo().name, (int)m_BandRows, m_SwapRGB565 ? "swapped" : "as drawn");
    return true;
}

bool PanelDisplay::Initialize(int32_t, int32_t)
{
    // The panel is started by DisplayPanelComponent, through Start().
    DEKI_LOG_ERROR("PanelDisplay::Initialize() called directly - use the DisplayPanelComponent instead");
    return false;
}

void PanelDisplay::Shutdown()
{
    if (!m_Initialized)
    {
        return;
    }
    // The worker finishes what it was given and stops before anything it
    // uses goes.
    SetAsyncPresent(false);
    m_IO.WaitPixels(0);
    if (m_TePin >= 0 || m_Scanline)
    {
        DekiRendering::RegisterScanOrderedDisplay(this, nullptr);
        DekiGpio::IDekiGPIO* gpio = DekiGpio::DekiGPIO::GetCurrent();
        if (gpio && m_TePin >= 0)
        {
            gpio->StopCounting(m_TePin);
        }
        m_TePin = -1;
        m_Scanline.reset();
    }
    FreeBands();
    for (uint16_t*& buffer : m_RenderBuffers)
    {
        Deki::Memory::Free(buffer);
        buffer = nullptr;
    }
    m_Initialized = false;
}

bool PanelDisplay::EnsureBands()
{
    if (m_Band[0] && m_Band[1])
    {
        return true;
    }
    const size_t bytes = static_cast<size_t>(m_PanelWidth) * m_BandRows * sizeof(uint16_t);
    for (uint16_t*& band : m_Band)
    {
        if (band)
        {
            continue;
        }
        // The panel reads the bands by DMA. Internal memory: some of the
        // chip's RAM is out of a peripheral's reach, and DMA from it corrupts
        // the display without an error.
        band = static_cast<uint16_t*>(Deki::Memory::AllocateDma(bytes, Deki::Memory::Internal));
        if (!band)
        {
            DEKI_LOG_ERROR("PanelDisplay: cannot allocate a %zu-byte band", bytes);
            FreeBands();
            return false;
        }
    }
    return true;
}

void PanelDisplay::FreeBands()
{
    for (uint16_t*& band : m_Band)
    {
        Deki::Memory::Free(band);
        band = nullptr;
    }
}

void PanelDisplay::Clear()
{
    if (!EnsureBands())
    {
        return;
    }
    const size_t rowBytes = static_cast<size_t>(m_PanelWidth) * sizeof(uint16_t);
    memset(m_Band[0], 0, rowBytes * m_BandRows);
    m_Controller.BeginWrite();
    m_Controller.SetWindow(0, 0, m_PanelWidth, m_PanelHeight);
    // One band of black, sent as often as the screen needs; it never changes,
    // so nothing waits for it to be free.
    for (int32_t y = 0; y < m_PanelHeight; y += m_BandRows)
    {
        const int32_t rows = std::min(m_BandRows, m_PanelHeight - y);
        m_IO.QueuePixels(m_Band[0], rowBytes * rows, y + rows < m_PanelHeight);
    }
    m_Controller.EndWrite();
}

void PanelDisplay::BlendOverlayRows(uint16_t* rows, size_t pitch, int width, int y0, int y1, bool swapped) const
{
    if (!HasOverlay())
    {
        return;
    }
    // The overlay covers the screen from its top left corner.
    const int w = std::min({ width, static_cast<int>(m_Width), static_cast<int>(m_ActiveOverlay->width) });
    const int end = std::min({ y1, static_cast<int>(m_Height), static_cast<int>(m_ActiveOverlay->height) });
    for (int y = y0 < 0 ? 0 : y0; y < end; ++y)
    {
        const int right = std::min(w, static_cast<int>(m_ActiveOverlay->spanRight[y]));
        const uint32_t* overlayRow = m_ActiveOverlay->buffer + static_cast<size_t>(y) * m_ActiveOverlay->width;
        uint16_t* dstRow = rows + static_cast<size_t>(y - y0) * pitch;
        for (int x = m_ActiveOverlay->spanLeft[y]; x < right; ++x)
        {
            const uint32_t argb = overlayRow[x];
            if ((argb & 0xFF000000) == 0)
            {
                continue;
            }
            const uint16_t bg = swapped ? SwapBytes16(dstRow[x]) : dstRow[x];
            const uint16_t out = BlendOverlayPixel(bg, argb);
            dstRow[x] = swapped ? SwapBytes16(out) : out;
        }
    }
}

void PanelDisplay::BeforeOverlayChange()
{
    // Turned, the worker blends the overlay into what it sends; otherwise
    // the drawing core blended it into its copy already.
    if (m_Turned)
    {
        WaitForPresents();
    }
}

void PanelDisplay::OverlayChanged()
{
    // A frame with dirty-rect tracking sends only the rows it drew in, and one
    // drawn in strips sends nothing of a strip where nothing moved: the rows
    // the overlay covers, or covered, would keep what the panel shows now.
    if (m_Initialized)
    {
        Deki::Engine::GetInstance().RequestFullRefresh();
    }
}

void PanelDisplay::PushRows(const uint8_t* framebuffer, int width, int height, Deki::ColorFormat format, int y0, int y1,
                            bool blendOverlay)
{
    m_Controller.BeginWrite();
    PushRowsBody(framebuffer, width, height, format, y0, y1, blendOverlay);
    m_Controller.EndWrite();
}

void PanelDisplay::PushRowsBody(const uint8_t* framebuffer, int width, int height, Deki::ColorFormat format, int y0,
                                int y1, bool blendOverlay)
{
    if (!EnsureBands())
    {
        return;
    }
    const int w = std::min(width, static_cast<int>(m_Width));
    y0 = std::max(y0, 0);
    y1 = std::min({ y1, height, static_cast<int>(m_Height) });
    if (y1 <= y0 || w <= 0)
    {
        return;
    }

    // One window for the whole run; the bands stream into it, each converted
    // while the one before it is on the wire, from the other buffer.
    m_Controller.SetWindow(0, y0, w, y1);
    for (int y = y0; y < y1; y += m_BandRows)
    {
        const int rows = std::min(static_cast<int>(m_BandRows), y1 - y);
        // The band about to be filled was queued two bands ago.
        m_IO.WaitPixels(1);
        uint16_t* band = m_Band[m_BandIndex];

        for (int i = 0; i < rows; ++i)
        {
            uint16_t* dst = band + static_cast<size_t>(i) * w;
            const int sy = y + i;
            if (format == Deki::ColorFormat::RGB565)
            {
                const uint16_t* src = reinterpret_cast<const uint16_t*>(framebuffer) + static_cast<size_t>(sy) * width;
                if (m_SwapRGB565)
                {
                    for (int x = 0; x < w; ++x)
                    {
                        dst[x] = SwapBytes16(src[x]);
                    }
                }
                else
                {
                    memcpy(dst, src, static_cast<size_t>(w) * sizeof(uint16_t));
                }
            }
            else if (format == Deki::ColorFormat::ARGB8888)
            {
                const uint32_t* src = reinterpret_cast<const uint32_t*>(framebuffer) + static_cast<size_t>(sy) * width;
                for (int x = 0; x < w; ++x)
                {
                    const uint32_t p = src[x];
                    const uint16_t v =
                        static_cast<uint16_t>((((p >> 16) & 0xF8) << 8) | (((p >> 8) & 0xFC) << 3) | ((p & 0xFF) >> 3));
                    dst[x] = m_WireBigEndian ? SwapBytes16(v) : v;
                }
            }
            else if (format == Deki::ColorFormat::RGB888)
            {
                const uint8_t* src = framebuffer + static_cast<size_t>(sy) * width * 3;
                for (int x = 0; x < w; ++x)
                {
                    const uint16_t v = static_cast<uint16_t>(((src[x * 3] & 0xF8) << 8) |
                                                             ((src[x * 3 + 1] & 0xFC) << 3) | (src[x * 3 + 2] >> 3));
                    dst[x] = m_WireBigEndian ? SwapBytes16(v) : v;
                }
            }
            else
            {
                memset(dst, 0, static_cast<size_t>(w) * sizeof(uint16_t));
            }
        }
        if (blendOverlay)
        {
            // The band is in the wire's byte order.
            BlendOverlayRows(band, w, w, y, y + rows, m_WireBigEndian);
        }

        m_IO.QueuePixels(band, static_cast<size_t>(rows) * w * sizeof(uint16_t), y + rows < y1);
        m_BandIndex = 1 - m_BandIndex;
        // The worker's last job finishes behind this band: its copy goes back
        // to the drawing core now, not when this whole job is queued.
        GiveBackPendingCopy();
    }
}

void PanelDisplay::PushRowsDirect(const uint16_t* framebuffer, int y0, int y1)
{
    m_Controller.BeginWrite();
    PushRowsDirectBody(framebuffer, y0, y1);
    // Ending the write waits for the last part: the frame is the engine's
    // again when this returns.
    m_Controller.EndWrite();
}

void PanelDisplay::PushRowsDirectBody(const uint16_t* frame, int y0, int y1)
{
    // As long a part as the bus takes: the fewer parts, the less each one's
    // start costs.
    const size_t rowBytes = static_cast<size_t>(m_PanelWidth) * sizeof(uint16_t);
    const int chunkRows = static_cast<int>(std::max<size_t>(1, m_IO.GetMaxChunkBytes() / rowBytes));
    m_Controller.SetWindow(0, y0, m_PanelWidth, y1);
    for (int y = y0; y < y1; y += chunkRows)
    {
        const int rows = std::min(chunkRows, y1 - y);
        m_IO.QueuePixels(frame + static_cast<size_t>(y) * m_PanelWidth, rowBytes * rows, y + rows < y1);
        // The worker's last job finishes behind this part.
        GiveBackPendingCopy();
    }
}

void PanelDisplay::CopyToWire(uint16_t* dst, const uint16_t* src, size_t pixels) const
{
    if (!m_SwapRGB565)
    {
        memcpy(dst, src, pixels * sizeof(uint16_t));
        return;
    }
    // Two pixels at a time, as SwapPixels does in place, when both ends are
    // 4-byte aligned (a frame, a frame copy, and whole rows of an even width
    // into them always are).
    if ((reinterpret_cast<uintptr_t>(dst) | reinterpret_cast<uintptr_t>(src)) & 3)
    {
        for (size_t i = 0; i < pixels; ++i)
        {
            dst[i] = SwapBytes16(src[i]);
        }
        return;
    }
    auto* d = reinterpret_cast<uint32_t*>(dst);
    const auto* s = reinterpret_cast<const uint32_t*>(src);
    for (size_t i = 0; i < pixels / 2; ++i)
    {
        const uint32_t v = s[i];
        d[i] = ((v >> 8) & 0x00FF00FF) | ((v & 0x00FF00FF) << 8);
    }
    if (pixels & 1)
    {
        dst[pixels - 1] = SwapBytes16(src[pixels - 1]);
    }
}

void PanelDisplay::Present(const uint8_t* framebuffer, int width, int height, Deki::ColorFormat format)
{
    if (!m_Initialized || !framebuffer)
    {
        return;
    }
    if (m_Turned)
    {
        if (format != Deki::ColorFormat::RGB565 || width != m_Width || height != m_Height)
        {
            if (!m_TurnWarned)
            {
                DEKI_LOG_ERROR(
                    "PanelDisplay: a turned panel is sent RGB565 frames of its own size only (got format %d, "
                    "%dx%d)",
                    static_cast<int>(format), width, height);
                m_TurnWarned = true;
            }
            return;
        }
        PresentTurned(reinterpret_cast<const uint16_t*>(framebuffer), width, 0, 0, width, height, 0, m_PanelHeight,
                      true, false);
        if (m_RenderBuffers[1])
        {
            m_RenderIndex = 1 - m_RenderIndex;
        }
        return;
    }
    // Async: the frame is copied for the worker, and the engine draws the
    // next while it is sent. Waits only when both copies are in use.
    // (The bands it is sent through are made here, on the drawing core: the
    // worker never allocates.)
    if (CanPresentAsync(framebuffer, width, height, format))
    {
        const int index = TakeCopy();
        // Copied in the wire's byte order (swapped as it is copied, at about
        // a copy's cost), so the worker sends the copy as it is, by DMA from
        // external memory, in parts as long as the bus takes.
        auto* copy = reinterpret_cast<uint16_t*>(m_Staging[index]);
        CopyToWire(copy, reinterpret_cast<const uint16_t*>(framebuffer), static_cast<size_t>(width) * height);
        BlendOverlayRows(copy, width, width, 0, height, m_WireBigEndian);
        Post(Job{ index, 0, height, false, false, true });
        return;
    }
    // Any other present goes after the worker's: it uses the bus and the
    // bands.
    WaitForPresents();
    WaitForRefresh();
    // Already in the wire's order, the panel's size and with nothing to blend
    // over it: the frame goes out as it is, with no copy.
    const bool direct =
        format == Deki::ColorFormat::RGB565 && !m_SwapRGB565 && !HasOverlay() && width == m_Width && height == m_Height;
    if (m_Presents++ == 0)
    {
        DEKI_LOG_INTERNAL("PanelDisplay: first present: fmt=%d %dx%d overlay=%d direct=%d", (int)format, width, height,
                          HasOverlay() ? 1 : 0, direct ? 1 : 0);
    }
    if (direct)
    {
        PushRowsDirect(reinterpret_cast<const uint16_t*>(framebuffer), 0, height);
    }
    else
    {
        PushRows(framebuffer, width, height, format, 0, height, true);
    }
    NotePartSent();
    if (m_RenderBuffers[1])
    {
        m_RenderIndex = 1 - m_RenderIndex;
    }
}

void PanelDisplay::PresentRegions(const uint8_t* framebuffer, int width, int height, Deki::ColorFormat format,
                                  const Deki::Rect* rects, int32_t count)
{
    PresentRegionsFrom(framebuffer, width, height, format, rects, count, true);
}

void PanelDisplay::PresentRegionsFrom(const uint8_t* framebuffer, int width, int height, Deki::ColorFormat format,
                                      const Deki::Rect* rects, int32_t count, bool frameStart)
{
    // Nothing changed on the panel, or (drawing in strips) every strip has
    // been sent already: nothing to wait for either, so the strip the worker
    // is still sending goes on while the next frame is drawn.
    if (!m_Initialized || !framebuffer || count == 0)
    {
        return;
    }
    if (m_Turned)
    {
        if (format != Deki::ColorFormat::RGB565 || width != m_Width || height != m_Height)
        {
            if (!m_TurnWarned)
            {
                DEKI_LOG_ERROR(
                    "PanelDisplay: a turned panel is sent RGB565 frames of its own size only (got format %d, "
                    "%dx%d)",
                    static_cast<int>(format), width, height);
                m_TurnWarned = true;
            }
            return;
        }
        // The panel rows the rectangles fall in: one run from the first to
        // the last, which the whole frame has every pixel of.
        int first = m_PanelHeight;
        int last = 0;
        for (int32_t i = 0; i < count; ++i)
        {
            if (rects[i].Empty())
            {
                continue;
            }
            int f = 0;
            int l = 0;
            PanelRowsOf(rects[i], &f, &l);
            first = std::min(first, f);
            last = std::max(last, l);
        }
        PresentTurned(reinterpret_cast<const uint16_t*>(framebuffer), width, 0, 0, width, height, first, last,
                      frameStart, false);
        if (m_RenderBuffers[1])
        {
            m_RenderIndex = 1 - m_RenderIndex;
        }
        return;
    }
    // One rectangle (a strip of a frame drawn in strips, or a frame where one
    // thing moved) goes to the worker, and the engine draws on while it is
    // sent.
    if (count == 1 && !rects[0].Empty() &&
        PresentRowsAsync(framebuffer, width, height, format, rects[0].top, rects[0].bottom, frameStart))
    {
        return;
    }
    WaitForPresents();
    if (frameStart)
    {
        WaitForRefresh();
    }

    // A window here spans whole rows, since the framebuffer's rows are only
    // contiguous at full width, so rectangles become row bands: widened to
    // the panel's row alignment, sorted and merged.
    const int32_t align = std::max<int32_t>(1, m_Controller.GetRowAlign());
    m_BandScratch.clear();
    for (int32_t i = 0; i < count; ++i)
    {
        const Deki::Rect& r = rects[i];
        if (r.Empty())
        {
            continue;
        }
        const int32_t top = r.top - (r.top % align);
        const int32_t bottom = ((r.bottom + align - 1) / align) * align;
        m_BandScratch.push_back(Deki::Rect{ 0, top, width, bottom });
    }
    std::sort(m_BandScratch.begin(), m_BandScratch.end(),
              [](const Deki::Rect& a, const Deki::Rect& b) { return a.top < b.top; });

    size_t out = 0;
    for (size_t i = 0; i < m_BandScratch.size(); ++i)
    {
        if (out > 0 && m_BandScratch[i].top <= m_BandScratch[out - 1].bottom)
        {
            m_BandScratch[out - 1].bottom = std::max(m_BandScratch[out - 1].bottom, m_BandScratch[i].bottom);
        }
        else
        {
            m_BandScratch[out++] = m_BandScratch[i];
        }
    }
    m_BandScratch.resize(out);

    const bool direct =
        format == Deki::ColorFormat::RGB565 && !m_SwapRGB565 && !HasOverlay() && width == m_Width && height == m_Height;
    for (const Deki::Rect& band : m_BandScratch)
    {
        if (direct)
        {
            PushRowsDirect(reinterpret_cast<const uint16_t*>(framebuffer), std::max<int>(band.top, 0),
                           std::min<int>(band.bottom, height));
        }
        else
        {
            PushRows(framebuffer, width, height, format, band.top, band.bottom, true);
        }
    }
    NotePartSent();
    if (m_RenderBuffers[1])
    {
        m_RenderIndex = 1 - m_RenderIndex;
    }
}

void PanelDisplay::GetDisplaySize(int32_t* width, int32_t* height) const
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

void* PanelDisplay::CreateUIOverlay(int32_t width, int32_t height)
{
    if (!m_Initialized || width <= 0 || height <= 0)
    {
        DEKI_LOG_ERROR("PanelDisplay::CreateUIOverlay: display not started, or no size");
        return nullptr;
    }
    auto* overlay = new UIOverlay();
    overlay->width = width;
    overlay->height = height;
    const size_t bytes = static_cast<size_t>(width) * height * sizeof(uint32_t);
    overlay->buffer = static_cast<uint32_t*>(Deki::Memory::Allocate(bytes, Deki::Memory::External));
    if (!overlay->buffer)
    {
        DEKI_LOG_ERROR("PanelDisplay::CreateUIOverlay: no room for the overlay (%zu bytes)", bytes);
        delete overlay;
        return nullptr;
    }
    memset(overlay->buffer, 0, bytes);
    overlay->spanLeft.assign(height, 0);
    overlay->spanRight.assign(height, 0);
    return overlay;
}

bool PanelDisplay::UpdateUIOverlay(void* overlay, int32_t x, int32_t y, int32_t width, int32_t height,
                                   const uint32_t* pixels)
{
    if (!overlay || !pixels)
    {
        return false;
    }
    auto* o = static_cast<UIOverlay*>(overlay);
    BeforeOverlayChange();
    if (x < 0 || y < 0 || width < 0 || height < 0 || x + width > o->width || y + height > o->height)
    {
        DEKI_LOG_WARNING("PanelDisplay::UpdateUIOverlay: bounds (%d,%d,%d,%d) outside the %dx%d overlay", x, y, width,
                         height, o->width, o->height);
        return false;
    }
    for (int32_t row = 0; row < height; ++row)
    {
        uint32_t* dest = &o->buffer[static_cast<size_t>(y + row) * o->width + x];
        memcpy(dest, &pixels[static_cast<size_t>(row) * width], static_cast<size_t>(width) * sizeof(uint32_t));
        MeasureOverlayRow(&o->buffer[static_cast<size_t>(y + row) * o->width], o->width, o->spanLeft[y + row],
                          o->spanRight[y + row]);
    }
    if (o == m_ActiveOverlay)
    {
        OverlayChanged();
    }
    return true;
}

bool PanelDisplay::UpdateUIOverlayRGB565A8(void*, int32_t, int32_t, int32_t, int32_t, const uint8_t*)
{
    return false;
}

void PanelDisplay::DestroyUIOverlay(void* overlay)
{
    if (!overlay)
    {
        return;
    }
    auto* o = static_cast<UIOverlay*>(overlay);
    BeforeOverlayChange();
    if (o == m_ActiveOverlay)
    {
        // The rows it covered are sent again without it.
        m_ActiveOverlay = nullptr;
        OverlayChanged();
    }
    Deki::Memory::Free(o->buffer);
    delete o;
}

void PanelDisplay::SetActiveUIOverlay(void* overlay)
{
    if (m_ActiveOverlay == static_cast<UIOverlay*>(overlay))
    {
        return;
    }
    BeforeOverlayChange();
    m_ActiveOverlay = static_cast<UIOverlay*>(overlay);
    OverlayChanged();
}

void PanelDisplay::ClearActiveUIOverlay()
{
    if (!HasOverlay())
    {
        return;
    }
    BeforeOverlayChange();
    memset(m_ActiveOverlay->buffer, 0,
           static_cast<size_t>(m_ActiveOverlay->width) * m_ActiveOverlay->height * sizeof(uint32_t));
    std::fill(m_ActiveOverlay->spanRight.begin(), m_ActiveOverlay->spanRight.end(), 0);
    OverlayChanged();
}

uint8_t* PanelDisplay::GetRenderBuffer(int32_t* width, int32_t* height)
{
    // Offered wherever it lives. With usePsram the engine draws straight into
    // PSRAM: slower to draw into than internal RAM, but a screen-sized buffer
    // there leaves too little internal RAM for the scene.
    if (!m_Initialized || !m_RenderBuffers[m_RenderIndex])
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
    return reinterpret_cast<uint8_t*>(m_RenderBuffers[m_RenderIndex]);
}

void PanelDisplay::SetBacklight(bool on)
{
    DekiGpio::IDekiGPIO* gpio = DekiGpio::DekiGPIO::GetCurrent();
    if (gpio && m_Options.backlightPin >= 0)
    {
        gpio->Write(m_Options.backlightPin, on != m_Options.backlightInvert);
        return;
    }
    // No backlight (an AMOLED): the panel's own brightness, set between
    // presents since it goes over the panel's bus.
    if (m_Initialized && m_Controller.HasBrightness())
    {
        WaitForPresents();
        m_Controller.SetBrightness(on ? m_Controller.GetInfo().onBrightness : 0);
    }
}

bool PanelDisplay::SendCommand(uint8_t cmd, const uint8_t* params, size_t n)
{
    if (!m_Initialized)
    {
        return false;
    }
    WaitForPresents();
    return m_Controller.SendCommand(cmd, params, n);
}

// ---- Async present ----------------------------------------------------------

void PanelDisplay::SetWorkerBackend(const WorkerBackend* backend)
{
    s_Worker = backend;
}

bool PanelDisplay::SetAsyncPresent(bool on)
{
    if (!on)
    {
        if (m_Async)
        {
            WaitForPresents();
            Post(Job{ 0, 0, 0, false, true });
            std::unique_lock<std::mutex> lock(m_Lock);
            m_CopyFree.wait(lock, [this] { return !m_WorkerRunning; });
        }
        for (uint8_t*& staging : m_Staging)
        {
            Deki::Memory::Free(staging);
            staging = nullptr;
        }
        FreeStrips();
        m_StagingBytes = 0;
        m_NextCopy = 0;
        m_Async = false;
        return true;
    }
    if (m_Async)
    {
        return true;
    }
    if (!m_Initialized)
    {
        DEKI_LOG_ERROR("PanelDisplay: async present needs a started display");
        return false;
    }
    if (!s_Worker || !s_Worker->start)
    {
        DEKI_LOG_INFO("PanelDisplay: this platform has no second core to send frames from; they are sent as they "
                      "are drawn");
        return false;
    }
    // Two frames in external memory: the worker sends one while the engine
    // copies the next frame into the other, then draws on into its own.
    const size_t bytes = Deki::FrameBufferBytes(Deki::ColorFormat::RGB565, m_Width, m_Height);
    for (uint8_t*& staging : m_Staging)
    {
        // DMA-capable: the worker sends straight from these.
        staging = static_cast<uint8_t*>(Deki::Memory::AllocateDma(bytes, Deki::Memory::External));
    }
    if (!m_Staging[0] || !m_Staging[1])
    {
        DEKI_LOG_INFO("PanelDisplay: no external memory for two %zu-byte frame copies; frames are sent as they are "
                      "drawn",
                      bytes);
        SetAsyncPresent(false);
        return false;
    }
    m_StagingBytes = bytes;
    {
        std::lock_guard<std::mutex> lock(m_Lock);
        m_JobHead = 0;
        m_JobCount = 0;
        m_FreeCopies = 2;
        m_WorkerRunning = true;
    }
    if (!s_Worker->start(&PanelDisplay::WorkerMain, this))
    {
        {
            std::lock_guard<std::mutex> lock(m_Lock);
            m_WorkerRunning = false;
        }
        DEKI_LOG_INFO("PanelDisplay: the present worker did not start (one core?); frames are sent as they are "
                      "drawn");
        SetAsyncPresent(false);
        return false;
    }
    m_Async = true;
    DEKI_LOG_INFO("PanelDisplay: frames are sent from the other core while the next is drawn");
    return true;
}

void PanelDisplay::WorkerMain(void* self)
{
    static_cast<PanelDisplay*>(self)->RunWorker();
}

void PanelDisplay::RunWorker()
{
    // The worker holds the bus from one job to the next while jobs keep
    // coming, so each job's window commands and first part are queued behind
    // the one before while it is still on the wire, and the wire never waits
    // between strips. A job's copy goes back to the drawing core once its
    // parts are sent, which is found after the next job is queued, or when
    // there is no next job: then everything is waited for, the copy given
    // back and the bus let go, so the SD card and the radio get it between
    // frames.
    bool held = false;
    uint32_t jobs = 0;
    bool waitedForRefresh = false;  // blocked since the last pause
    for (;;)
    {
        Job job{};
        {
            std::unique_lock<std::mutex> lock(m_Lock);
            if (m_JobCount == 0 && held)
            {
                lock.unlock();
                m_Controller.EndWrite();  // waits for everything queued
                held = false;
                GiveBackPendingCopy();
                continue;
            }
            m_JobReady.wait(lock, [this] { return m_JobCount > 0; });
            job = m_Jobs[m_JobHead];
            m_JobHead = (m_JobHead + 1) % 3;
            --m_JobCount;
        }
        if (job.stop)
        {
            break;  // posted after WaitForPresents: nothing is held or pending
        }
        // At full speed the next job is always waiting, so the worker never
        // blocks and the core's idle task never runs, which a task watchdog
        // reports. A pause every 30 jobs lets it; what is queued goes on.
        // Waiting for the panel's refresh blocks every frame, and lets the
        // idle task run then: a pause would only make a frame miss its
        // refresh.
        if (++jobs % 30 == 0)
        {
            if (!waitedForRefresh && s_Worker && s_Worker->yield)
            {
                s_Worker->yield();
            }
            waitedForRefresh = false;
        }
        // A frame waiting for the panel's refresh starts with the last one
        // sent and the bus let go, so the wait holds up nothing else on it.
        if (job.frameStart && IsTearFree())
        {
            if (held)
            {
                m_Controller.EndWrite();
                held = false;
            }
            GiveBackPendingCopy();
            WaitForRefresh();
            waitedForRefresh = true;
        }
        if (!held)
        {
            m_Controller.BeginWrite();
            held = true;
        }
        if (job.raw)
        {
            const uint16_t* src =
                job.strip ? m_Strip[job.index] : reinterpret_cast<const uint16_t*>(m_Staging[job.index]);
            PushPanelRowsBody(src, job.srcStride, job.srcLeft, job.srcTop, job.top, job.bottom);
        }
        else if (job.strip)
        {
            SendStripBody(m_Strip[job.index], job.top, job.bottom - job.top, !job.wire);
        }
        else
        {
            // In the wire's order, with the overlay blended over it, as the
            // drawing core copied it.
            PushRowsDirectBody(reinterpret_cast<const uint16_t*>(m_Staging[job.index]), job.top, job.bottom);
        }
        // Given back as this job's first part was queued, unless it queued
        // none.
        GiveBackPendingCopy();
        m_PendingCopy = true;
        m_PendingEnd = m_IO.GetQueuedCount();
        NotePartSent();
    }
    {
        std::lock_guard<std::mutex> lock(m_Lock);
        m_WorkerRunning = false;
    }
    m_CopyFree.notify_all();
}

void PanelDisplay::GiveBackPendingCopy()
{
    if (!m_PendingCopy)
    {
        return;
    }
    // Everything queued after the last job's end may still be in flight; the
    // last job's own parts must not.
    m_IO.WaitPixels(m_IO.GetQueuedCount() - m_PendingEnd);
    m_PendingCopy = false;
    {
        std::lock_guard<std::mutex> lock(m_Lock);
        ++m_FreeCopies;
    }
    m_CopyFree.notify_all();
}

void PanelDisplay::Post(const Job& job)
{
    {
        std::lock_guard<std::mutex> lock(m_Lock);
        m_Jobs[(m_JobHead + m_JobCount) % 3] = job;
        ++m_JobCount;
    }
    m_JobReady.notify_one();
}

int PanelDisplay::TakeCopy()
{
    std::unique_lock<std::mutex> lock(m_Lock);
    m_CopyFree.wait(lock, [this] { return m_FreeCopies > 0; });
    --m_FreeCopies;
    const int index = m_NextCopy;
    m_NextCopy = 1 - m_NextCopy;
    return index;
}

void PanelDisplay::WaitForPresents()
{
    // m_Async is only changed on the drawing core, which this runs on.
    if (!m_Async)
    {
        return;
    }
    // Both copies free: nothing queued or being sent.
    std::unique_lock<std::mutex> lock(m_Lock);
    m_CopyFree.wait(lock, [this] { return m_FreeCopies == 2 || !m_WorkerRunning; });
}

bool PanelDisplay::CanPresentAsync(const uint8_t* framebuffer, int width, int height, Deki::ColorFormat format) const
{
    return m_Async && framebuffer && format == Deki::ColorFormat::RGB565 && width == m_Width && height == m_Height &&
           Deki::FrameBufferBytes(format, width, height) <= m_StagingBytes;
}

bool PanelDisplay::EnsureStrips(int rows)
{
    if (rows <= m_StripRows && m_Strip[0] && m_Strip[1])
    {
        return true;
    }
    if (m_StripFailedRows > 0 && rows >= m_StripFailedRows)
    {
        return false;
    }
    const size_t bytes = static_cast<size_t>(m_PanelWidth) * rows * sizeof(uint16_t);
    if (bytes > kMaxStripBytes)
    {
        return false;
    }
    // The new pair is made before the old one goes, and if it cannot be the
    // old one stays, for the strips it fits. Asking again every strip once
    // internal memory is in pieces cost more than the copies save.
    uint16_t* grown[2] = { nullptr, nullptr };
    for (uint16_t*& strip : grown)
    {
        strip = static_cast<uint16_t*>(Deki::Memory::AllocateDma(bytes, Deki::Memory::Internal));
        if (!strip)
        {
            DEKI_LOG_WARNING("PanelDisplay: no internal DMA memory for two %zu-byte strip copies; strips of %d rows "
                             "or more go through external memory",
                             bytes, rows);
            Deki::Memory::Free(grown[0]);
            m_StripFailedRows = rows;
            return false;
        }
    }
    // The worker may still be sending from the old copies.
    WaitForPresents();
    FreeStrips();
    m_Strip[0] = grown[0];
    m_Strip[1] = grown[1];
    m_StripRows = rows;
    DEKI_LOG_INFO("PanelDisplay: strips of up to %d rows are sent from the other core while the next is drawn", rows);
    return true;
}

bool PanelDisplay::LendsStripBuffers() const
{
    return m_Async && m_Turned && IsTearFree();
}

uint8_t* PanelDisplay::GetStripBuffer(size_t bytes)
{
    // A strip of columns holds whole panel rows: bytes / one panel row's.
    const size_t rowBytes = static_cast<size_t>(m_PanelWidth) * sizeof(uint16_t);
    if (!LendsStripBuffers() || m_Lent >= 0 || rowBytes == 0 || bytes % rowBytes != 0 ||
        !EnsureStrips(static_cast<int>(bytes / rowBytes)))
    {
        return nullptr;
    }
    m_Lent = TakeCopy();
    return reinterpret_cast<uint8_t*>(m_Strip[m_Lent]);
}

void PanelDisplay::GiveBackCopy(int index)
{
    {
        std::lock_guard<std::mutex> lock(m_Lock);
        m_NextCopy = index;
        ++m_FreeCopies;
    }
    m_CopyFree.notify_all();
}

int32_t PanelDisplay::GetMaxStripPixels() const
{
    // EnsureStrips counts panel rows; the strips it is sent are whole ones.
    return m_StripFailedRows > 0 ? (m_StripFailedRows - 1) * m_PanelWidth : 0;
}

void PanelDisplay::FreeStrips()
{
    for (uint16_t*& strip : m_Strip)
    {
        Deki::Memory::Free(strip);
        strip = nullptr;
    }
    m_StripRows = 0;
}

void PanelDisplay::SendStripBody(uint16_t* strip, int top, int rows, bool swap)
{
    const int w = m_PanelWidth;
    const size_t rowBytes = static_cast<size_t>(w) * sizeof(uint16_t);
    // The strip goes out in chunks, each queued as soon as it is ready. With
    // bytes to swap, the chunks are bands' size and each is swapped while the
    // one before it is on the wire, so a strip costs about its transfer
    // instead of its swap and then its transfer.
    swap = swap && m_SwapRGB565;
    int chunkRows = static_cast<int>(std::max<size_t>(1, m_IO.GetMaxChunkBytes() / rowBytes));
    if (swap)
    {
        chunkRows = std::min(chunkRows, static_cast<int>(m_BandRows));
    }
    m_Controller.SetWindow(0, top, w, top + rows);
    for (int y = 0; y < rows; y += chunkRows)
    {
        const int n = std::min(chunkRows, rows - y);
        uint16_t* chunk = strip + static_cast<size_t>(y) * w;
        if (swap)
        {
            SwapPixels(chunk, static_cast<size_t>(w) * n);
        }
        m_IO.QueuePixels(chunk, rowBytes * n, y + n < rows);
        GiveBackPendingCopy();
    }
}

bool PanelDisplay::PresentRowsAsync(const uint8_t* framebuffer, int width, int height, Deki::ColorFormat format,
                                    int top, int bottom, bool frameStart)
{
    if (!CanPresentAsync(framebuffer, width, height, format))
    {
        return false;
    }
    // Widened to the panel's row alignment, as the synchronous path does.
    const int align = std::max<int>(1, m_Controller.GetRowAlign());
    top -= top % align;
    bottom = ((bottom + align - 1) / align) * align;
    top = std::max(top, 0);
    bottom = std::min(bottom, height);
    if (bottom <= top)
    {
        return true;
    }

    // A strip that fits the strip copies goes there and is sent from them as
    // it is. Anything taller goes to the same rows of a whole-frame copy,
    // sent through the bands like any other frame.
    const bool strip = EnsureStrips(bottom - top);
    const int index = TakeCopy();
    const size_t pitch = static_cast<size_t>(width) * sizeof(uint16_t);
    const auto* rows = reinterpret_cast<const uint16_t*>(framebuffer + static_cast<size_t>(top) * pitch);
    const size_t pixels = static_cast<size_t>(bottom - top) * width;
    if (strip)
    {
        // One memcpy in memory's byte order: the worker swaps it in place, a
        // chunk at a time while the chunk before is on the wire, so the
        // drawing core draws on sooner.
        uint16_t* copy = m_Strip[index];
        memcpy(copy, rows, pixels * sizeof(uint16_t));
        BlendOverlayRows(copy, width, width, top, bottom, Deki::kRGB565BigEndian);
    }
    else
    {
        // Into the same rows of a frame copy, in the wire's order, as a whole
        // frame is.
        auto* copy = reinterpret_cast<uint16_t*>(m_Staging[index] + static_cast<size_t>(top) * pitch);
        CopyToWire(copy, rows, pixels);
        BlendOverlayRows(copy, width, width, top, bottom, m_WireBigEndian);
    }
    Post(Job{ index, top, bottom, strip, false, frameStart });
    return true;
}

// ---- Tear-free --------------------------------------------------------------

namespace
{
// A panel refreshes at 30 to 60 Hz: a TE line silent for this long is not
// wired, or not pulsing.
constexpr uint32_t kRefreshTimeoutMs = 50;
constexpr uint8_t kCmdTeOn = 0x35;
constexpr uint8_t kMadctlMY = 0x80;
constexpr uint8_t kMadctlMV = 0x20;
}  // namespace

void PanelDisplay::StartTearFree(int32_t tePin, bool scanline)
{
    m_TePin = -1;
    m_Scanline.reset();
    m_TeLost = false;
    m_Turned = false;
    m_ToScreen = PixelMap{};
    m_ToPanel = PixelMap{};
    m_ScanOrder = DekiRendering::ScanOrder::RowsDown;
    if (tePin < 0 && !scanline)
    {
        return;
    }
    // TE, the scanline and the turn here take MIPI DCS commands and MADCTL
    // bits.
    if (!m_Controller.GetInfo().commands.IsMipiDcs())
    {
        DEKI_LOG_WARNING("PanelDisplay: the %s cannot tell its refresh; frames are sent as they are drawn",
                         m_Controller.GetInfo().name);
        return;
    }
    if (tePin >= 0)
    {
        DekiGpio::IDekiGPIO* gpio = DekiGpio::DekiGPIO::GetCurrent();
        if (!gpio || !gpio->CountEdges(tePin, DekiGpio::Edge::Rising, DekiGpio::Pull::None))
        {
            DEKI_LOG_WARNING("PanelDisplay: the TE pin (GPIO %d) cannot be read here; frames are sent as they are "
                             "drawn",
                             static_cast<int>(tePin));
            return;
        }
        m_TePin = tePin;
        // TE on, pulsing once a refresh, as the refresh starts.
        const uint8_t verticalOnly = 0x00;
        m_Controller.SendCommand(kCmdTeOn, &verticalOnly, 1);
    }
    else
    {
        auto clock = std::make_unique<ScanlineClock>(m_IO, &PanelDisplay::Micros,
                                                     s_Worker && s_Worker->yield ? s_Worker->yield : nullptr);
        if (Micros() == 0 || !clock->Calibrate())
        {
            DEKI_LOG_INFO("PanelDisplay: the %s's scanline cannot be read here (is it wired to answer, or does it "
                          "answer on MOSI: readsOnMosi?); frames are sent as they are drawn, and can tear",
                          m_Controller.GetInfo().name);
            return;
        }
        m_RefreshUs = clock->GetPeriodUs();
        m_Scanline = std::move(clock);
        DEKI_LOG_INFO("PanelDisplay: the panel refreshes at %.1f Hz, from its scanline", 1e6 / m_RefreshUs);
    }

    // Shown turned or upside down, the screen's rows are not the panel's: the
    // controller goes back to its own orientation and the screen is turned
    // here, so what is sent is in the order the panel refreshes.
    const uint8_t rotation = m_Controller.GetScreenRotation();
    const ControllerInfo& info = m_Controller.GetInfo();
    const uint8_t own = info.madctl[0];
    if (rotation != 0)
    {
        if (own & (kMadctlMY | kMadctlMV))
        {
            DEKI_LOG_WARNING("PanelDisplay: the %s's own orientation is not its refresh order; the screen is turned "
                             "by the controller and can tear",
                             info.name);
        }
        else
        {
            const PanelConfig& c = m_Controller.GetConfig();
            const PixelMap screenToPanel = MadctlMap(info.madctl[rotation & 7], c.panelWidth, c.panelHeight);
            const PixelMap ownToPanel = MadctlMap(own, c.panelWidth, c.panelHeight);
            m_ToPanel = screenToPanel.Then(ownToPanel.Inverse());
            m_ToScreen = m_ToPanel.Inverse();
            m_Controller.WriteInOwnOrientation();
            m_PanelWidth = m_Controller.GetWidth();
            m_PanelHeight = m_Controller.GetHeight();
            m_Turned = true;
        }
    }
    // How the screen goes as the panel's rows go: the order strips are asked
    // for in.
    if (m_ToScreen.yy != 0)
    {
        m_ScanOrder = m_ToScreen.yy > 0 ? DekiRendering::ScanOrder::RowsDown : DekiRendering::ScanOrder::RowsUp;
    }
    else
    {
        m_ScanOrder =
            m_ToScreen.xy > 0 ? DekiRendering::ScanOrder::ColumnsRight : DekiRendering::ScanOrder::ColumnsLeft;
    }
    DekiRendering::RegisterScanOrderedDisplay(this, this);
    static const char* const kOrders[] = { "rows down", "rows up", "columns left to right", "columns right to left" };
    char source[24];
    if (m_TePin >= 0)
    {
        snprintf(source, sizeof(source), "TE on GPIO %d", static_cast<int>(m_TePin));
    }
    else
    {
        snprintf(source, sizeof(source), "its scanline");
    }
    DEKI_LOG_INFO("PanelDisplay: frames start on the panel's refresh (%s) and go in its own row order: %s of the "
                  "screen%s",
                  source, kOrders[static_cast<int>(m_ScanOrder)], m_Turned ? ", turned here" : "");
}

uint64_t PanelDisplay::Micros()
{
    return s_Worker && s_Worker->micros ? s_Worker->micros() : 0;
}

void PanelDisplay::NotePartSent()
{
    m_LastPartUs = Micros();
}

// A refresh takes P; a frame takes W to go out, row 0 first, faster than the
// refresh goes (W < P). Started on the refresh's start, the write stays ahead
// of it: that refresh shows the new frame. Started once the refresh has run
// P - W or more, the write cannot catch it up before it ends, so that refresh
// shows the old frame everywhere; and at the next refresh's start the write
// is ahead, so that one shows the new frame. Anywhere in between, the write
// overtakes the refresh, and that refresh shows a seam.
//
// So a frame whose refresh has not started yet waits for it (and the panel
// sets the pace), and one that missed it waits only until P - W into it
// instead of the next start: a frame that takes a little longer than a
// refresh to draw then costs a little more time, not a whole refresh more.
//
// W is this frame's, which is not known until it is sent. WaitForRefresh
// takes the longest any frame has taken: a late frame is often the one that
// changes everything (a new scene, loaded in that frame), and the frames
// before it, which changed a little, went out far faster.
namespace
{
// A frame's write is not quite as steady as a refresh: a margin. It also
// covers the blanking a refresh starts with, before its first row.
constexpr uint32_t kTearMarginUs = 1000;
}  // namespace

uint32_t PanelDisplay::LongestWriteUs(uint32_t longestUs, uint32_t writeUs, uint32_t refreshUs)
{
    if (refreshUs != 0 && writeUs >= 2 * refreshUs)
    {
        return longestUs;
    }
    return std::max(longestUs, writeUs);
}

bool PanelDisplay::WritesBehind(uint32_t refreshUs, uint32_t writeUs)
{
    return writeUs != 0 && writeUs + kTearMarginUs >= refreshUs;
}

// A write that takes about a refresh or longer (a whole screen on one SPI
// line, which goes at much the refresh's pace) cannot get ahead of it, but
// can go behind: started just after the refresh begins, past its blanking,
// the write never catches the refresh's rows up, so that refresh shows the
// old frame; and if it ends within two refreshes, the next refresh reaches
// each row after it is written, and shows the new frame whole.
uint32_t PanelDisplay::TearFreeStartUs(uint32_t refreshUs, uint32_t writeUs)
{
    if (writeUs == 0)
    {
        return refreshUs;
    }
    if (writeUs + kTearMarginUs >= 2 * refreshUs)
    {
        return 0;
    }
    return (writeUs < refreshUs ? refreshUs - writeUs : 0) + kTearMarginUs;
}

void PanelDisplay::WaitForRefresh()
{
    if (m_Scanline)
    {
        WaitForRefreshByScanline();
        return;
    }
    if (m_TePin < 0 || m_TeLost)
    {
        return;
    }
    DekiGpio::IDekiGPIO* gpio = DekiGpio::DekiGPIO::GetCurrent();
    const uint64_t now = Micros();
    if (m_FrameStartUs != 0 && m_LastPartUs > m_FrameStartUs)
    {
        m_LongestWriteUs =
            LongestWriteUs(m_LongestWriteUs, static_cast<uint32_t>(m_LastPartUs - m_FrameStartUs), m_RefreshUs);
    }
    uint32_t age = 0;
    if (gpio && m_RefreshUs != 0 && now != 0 && gpio->TimeSinceEdge(m_TePin, age) && age < 2 * m_RefreshUs)
    {
        uint32_t opens = TearFreeStartUs(m_RefreshUs, m_LongestWriteUs);
        if (gpio->TakeEdges(m_TePin) == 0)
        {
            // The frame before started in this refresh: the next start, and
            // past its blanking for a write that goes behind the refresh.
            WaitForRefreshStart();
            if (!WritesBehind(m_RefreshUs, m_LongestWriteUs) || !gpio->TimeSinceEdge(m_TePin, age))
            {
                opens = 0;
            }
        }
        uint32_t was = age;
        while (age < opens && age >= was && !m_TeLost)
        {
            // Ticks while there is more than one to wait, then the rest.
            if (opens - age > 2000 && s_Worker && s_Worker->yield)
            {
                s_Worker->yield();
            }
            was = age;
            if (!gpio->TimeSinceEdge(m_TePin, age))
            {
                break;
            }
        }
        gpio->TakeEdges(m_TePin);  // counted from this frame's start
        m_FrameStartUs = Micros();
        return;
    }
    WaitForRefreshStart();
    m_FrameStartUs = Micros();
}

// WaitForRefresh's rule, on the scanline clock.
void PanelDisplay::WaitForRefreshByScanline()
{
    const uint64_t now = Micros();
    if (m_FrameStartUs != 0 && m_LastPartUs > m_FrameStartUs)
    {
        m_LongestWriteUs =
            LongestWriteUs(m_LongestWriteUs, static_cast<uint32_t>(m_LastPartUs - m_FrameStartUs), m_RefreshUs);
    }
    const uint32_t age = m_Scanline->AgeUs(now);
    const uint32_t opens = TearFreeStartUs(m_Scanline->GetPeriodUs(), m_LongestWriteUs);
    if ((m_FrameStartUs != 0 && now - m_FrameStartUs <= age) || opens >= m_Scanline->GetPeriodUs())
    {
        // The frame before started in this refresh, or a frame's write is
        // not known yet: the next start, and past its blanking for a write
        // that goes behind the refresh.
        m_Scanline->WaitForStart();
        if (WritesBehind(m_Scanline->GetPeriodUs(), m_LongestWriteUs))
        {
            m_Scanline->WaitForAge(opens);
        }
    }
    else
    {
        m_Scanline->WaitForAge(opens);
    }
    m_FrameStartUs = Micros();
}

void PanelDisplay::WaitForRefreshStart()
{
    DekiGpio::IDekiGPIO* gpio = DekiGpio::DekiGPIO::GetCurrent();
    if (gpio && gpio->WaitForEdge(m_TePin, kRefreshTimeoutMs))
    {
        // The panel's refresh rate, said once: the TE line's edges over two
        // seconds, as the platform counts them.
        if (m_RefreshSince == 0)
        {
            gpio->TakeEdges(m_TePin);
            m_RefreshSince = Deki::Time::GetTime() | 1;
        }
        else if (!m_RefreshSaid)
        {
            const uint32_t elapsed = Deki::Time::GetElapsed(m_RefreshSince);
            if (elapsed >= 2000)
            {
                const uint32_t edges = gpio->TakeEdges(m_TePin);
                DEKI_LOG_INFO("PanelDisplay: the panel refreshes at %.1f Hz", 1000.0 * edges / elapsed);
                if (edges > 0)
                {
                    m_RefreshUs = static_cast<uint32_t>(1000ull * elapsed / edges);
                }
                m_RefreshSaid = true;
            }
        }
        return;
    }
    m_TeLost = true;
    DEKI_LOG_WARNING("PanelDisplay: no refresh pulse on the TE pin (GPIO %d) in %u ms; frames are sent without waiting "
                     "for one, and can tear",
                     static_cast<int>(m_TePin), static_cast<unsigned>(kRefreshTimeoutMs));
}

void PanelDisplay::PanelRowsOf(const Deki::Rect& rect, int* first, int* last) const
{
    int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    m_ToPanel.Apply(rect.left, rect.top, x0, y0);
    m_ToPanel.Apply(rect.right - 1, rect.bottom - 1, x1, y1);
    *first = std::clamp<int>(std::min(y0, y1), 0, m_PanelHeight);
    *last = std::clamp<int>(std::max(y0, y1) + 1, 0, m_PanelHeight);
}

void PanelDisplay::GatherPanelRows(uint16_t* dst, const uint16_t* src, int32_t stride, int32_t left, int32_t top,
                                   int first, int last) const
{
    const PixelMap& t = m_ToScreen;
    // Along a panel row the screen moves by (t.xx, t.yx): one step through
    // the source.
    const ptrdiff_t step = t.xx + static_cast<ptrdiff_t>(t.yx) * stride;
    const int w = m_PanelWidth;
    const UIOverlay* overlay = HasOverlay() ? m_ActiveOverlay : nullptr;
    // Along a panel row the source steps a whole screen row (a turned
    // screen): the band is read a screen row at a time instead, a run of
    // neighbouring pixels from each, so a frame in external memory is read a
    // cache line at a time rather than a pixel per line.
    if (!overlay && (step == stride || step == -static_cast<ptrdiff_t>(stride)))
    {
        const int rows = last - first;
        int32_t sx0 = 0, sy0 = 0, sx1 = 0, sy1 = 0;
        t.Apply(0, first, sx0, sy0);
        t.Apply(0, first + 1, sx1, sy1);
        // Panel rows step through the source by this much.
        const ptrdiff_t rowStep = (sx1 - sx0) + static_cast<ptrdiff_t>(sy1 - sy0) * stride;
        const uint16_t* base = src + static_cast<ptrdiff_t>(sy0 - top) * stride + (sx0 - left);
        for (int k = 0; k < w; ++k, base += step)
        {
            const uint16_t* p = base;
            uint16_t* d = dst + k;
            if (m_SwapRGB565)
            {
                for (int r = 0; r < rows; ++r, p += rowStep, d += w)
                {
                    *d = SwapBytes16(*p);
                }
            }
            else
            {
                for (int r = 0; r < rows; ++r, p += rowStep, d += w)
                {
                    *d = *p;
                }
            }
        }
        return;
    }
    for (int py = first; py < last; ++py)
    {
        int32_t sx = 0, sy = 0;
        t.Apply(0, py, sx, sy);
        const uint16_t* p = src + static_cast<ptrdiff_t>(sy - top) * stride + (sx - left);
        uint16_t* d = dst + static_cast<size_t>(py - first) * w;
        if (m_SwapRGB565)
        {
            for (int k = 0; k < w; ++k, p += step)
            {
                d[k] = SwapBytes16(*p);
            }
        }
        else
        {
            for (int k = 0; k < w; ++k, p += step)
            {
                d[k] = *p;
            }
        }
        if (!overlay)
        {
            continue;
        }
        // The overlay is in screen pixels, from its top-left corner.
        for (int k = 0; k < w; ++k)
        {
            const int32_t x = sx + k * t.xx;
            const int32_t y = sy + k * t.yx;
            if (y < 0 || y >= overlay->height || x < overlay->spanLeft[y] || x >= overlay->spanRight[y])
            {
                continue;
            }
            const uint32_t argb = overlay->buffer[static_cast<size_t>(y) * overlay->width + x];
            if ((argb & 0xFF000000) == 0)
            {
                continue;
            }
            const uint16_t bg = m_WireBigEndian ? SwapBytes16(d[k]) : d[k];
            const uint16_t out = BlendOverlayPixel(bg, argb);
            d[k] = m_WireBigEndian ? SwapBytes16(out) : out;
        }
    }
}

void PanelDisplay::PushPanelRowsBody(const uint16_t* src, int32_t stride, int32_t left, int32_t top, int first,
                                     int last)
{
    if (!EnsureBands())
    {
        return;
    }
    const size_t rowBytes = static_cast<size_t>(m_PanelWidth) * sizeof(uint16_t);
    m_Controller.SetWindow(0, first, m_PanelWidth, last);
    for (int y = first; y < last; y += m_BandRows)
    {
        const int rows = std::min(static_cast<int>(m_BandRows), last - y);
        // The band about to be filled was queued two bands ago.
        m_IO.WaitPixels(1);
        uint16_t* band = m_Band[m_BandIndex];
        GatherPanelRows(band, src, stride, left, top, y, y + rows);
        m_IO.QueuePixels(band, rowBytes * rows, y + rows < last);
        m_BandIndex = 1 - m_BandIndex;
        GiveBackPendingCopy();
    }
}

void PanelDisplay::PresentTurned(const uint16_t* src, int32_t stride, int32_t left, int32_t top, int32_t srcWidth,
                                 int32_t srcHeight, int first, int last, bool frameStart, bool stripCopy, int lent)
{
    if (last <= first)
    {
        return;
    }
    if (m_Async)
    {
        // Copied as it was drawn, and turned on the worker, a band at a time
        // while the band before it is on the wire: turning reads the screen
        // across its rows, which costs the drawing core several milliseconds a
        // frame and the worker nothing it was not waiting anyway. A strip goes
        // to one of the internal strip copies, anything else to a frame copy.
        // (The worker never allocates: its bands are made here, after the
        // strip copies, which need the bigger blocks of internal memory.)
        const size_t pixels = static_cast<size_t>(srcWidth) * srcHeight;
        const bool strip = lent >= 0 || (stripCopy && pixels % m_PanelWidth == 0 &&
                                         EnsureStrips(static_cast<int>(pixels / m_PanelWidth)));
        if (!EnsureBands())
        {
            if (lent >= 0)
            {
                GiveBackCopy(lent);
            }
            return;
        }
        const int index = lent >= 0 ? lent : TakeCopy();
        uint16_t* dst = strip ? m_Strip[index] : reinterpret_cast<uint16_t*>(m_Staging[index]);
        if (lent >= 0)
        {
            // Drawn in place.
        }
        else if (stride == srcWidth)
        {
            memcpy(dst, src, pixels * sizeof(uint16_t));
        }
        else
        {
            for (int32_t y = 0; y < srcHeight; ++y)
            {
                memcpy(dst + static_cast<size_t>(y) * srcWidth, src + static_cast<size_t>(y) * stride,
                       static_cast<size_t>(srcWidth) * sizeof(uint16_t));
            }
        }
        Job job{ index, first, last, strip, false, frameStart };
        job.raw = true;
        job.srcLeft = left;
        job.srcTop = top;
        job.srcStride = srcWidth;
        Post(job);
        return;
    }
    if (frameStart)
    {
        WaitForRefresh();
    }
    m_Controller.BeginWrite();
    PushPanelRowsBody(src, stride, left, top, first, last);
    m_Controller.EndWrite();
    NotePartSent();
}

void PanelDisplay::PresentStrip(const DekiRendering::ScanStrip& s)
{
    // A strip drawn in a lent copy: it is sent from there, or, with nothing
    // to send, given back.
    const int lent = m_Lent >= 0 && s.pixels == reinterpret_cast<const uint8_t*>(m_Strip[m_Lent]) ? m_Lent : -1;
    m_Lent = -1;
    if (!m_Initialized || !s.pixels || s.changed.Empty() || (lent >= 0 && !m_Turned))
    {
        if (lent >= 0)
        {
            GiveBackCopy(lent);
        }
        return;
    }
    if (!m_Turned)
    {
        // Strips of the panel's own rows: rows [strip.top, strip.bottom) of a
        // frame the screen's width, handed over as that frame.
        if (s.stride != m_Width || s.strip.left != 0)
        {
            DEKI_LOG_ERROR("PanelDisplay: a strip of rows must be the screen's width");
            return;
        }
        const size_t bpp = Deki::FrameBufferBytes(s.format, 1, 1);
        const auto* frameBase = reinterpret_cast<const uint8_t*>(reinterpret_cast<uintptr_t>(s.pixels) -
                                                                 static_cast<uintptr_t>(s.strip.top) * s.stride * bpp);
        PresentRegionsFrom(frameBase, m_Width, m_Height, s.format, &s.changed, 1, s.firstOfFrame);
        return;
    }
    if (s.format != Deki::ColorFormat::RGB565)
    {
        if (!m_TurnWarned)
        {
            DEKI_LOG_ERROR("PanelDisplay: a turned panel is sent RGB565 strips only (got format %d)",
                           static_cast<int>(s.format));
            m_TurnWarned = true;
        }
        if (lent >= 0)
        {
            GiveBackCopy(lent);
        }
        return;
    }
    // Whole panel rows go out, so the strip must hold every screen pixel of
    // the panel rows it falls in: a strip cut along the scan order does.
    int first = 0;
    int last = 0;
    PanelRowsOf(s.strip, &first, &last);
    int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    m_ToScreen.Apply(0, first, x0, y0);
    m_ToScreen.Apply(m_PanelWidth - 1, last - 1, x1, y1);
    if (std::min(x0, x1) < s.strip.left || std::max(x0, x1) >= s.strip.right || std::min(y0, y1) < s.strip.top ||
        std::max(y0, y1) >= s.strip.bottom)
    {
        if (!m_TurnWarned)
        {
            DEKI_LOG_ERROR("PanelDisplay: a strip not cut along the panel's rows cannot be sent turned");
            m_TurnWarned = true;
        }
        if (lent >= 0)
        {
            GiveBackCopy(lent);
        }
        return;
    }
    int changedFirst = 0;
    int changedLast = 0;
    PanelRowsOf(s.changed, &changedFirst, &changedLast);
    first = std::max(first, changedFirst);
    last = std::min(last, changedLast);
    PresentTurned(reinterpret_cast<const uint16_t*>(s.pixels), s.stride, s.strip.left, s.strip.top, s.strip.Width(),
                  s.strip.Height(), first, last, s.firstOfFrame, true, lent);
}

}  // namespace DekiDisplay
