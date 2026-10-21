#include "PanelDisplay.h"

#include "DekiGPIO.h"  // from deki-gpio
#include <deki/LogSystem.h>
#include <deki/providers/Memory.h>

#include <algorithm>
#include <cstring>

namespace DekiDisplay
{

namespace
{

inline uint16_t SwapBytes16(uint16_t v)
{
    return static_cast<uint16_t>((v >> 8) | (v << 8));
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

PanelDisplay::PanelDisplay(PanelController& controller, IPanelIO& io) : m_Controller(controller), m_IO(io) {}

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
    m_WireBigEndian = m_IO.PixelsBigEndian();
    m_SwapRGB565 = m_WireBigEndian != Deki::kRGB565BigEndian;

    // A band is at most one transfer, and a whole number of rows.
    const size_t rowBytes = static_cast<size_t>(m_Width) * sizeof(uint16_t);
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
    m_IO.WaitPixels(0);
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
    const size_t bytes = static_cast<size_t>(m_Width) * m_BandRows * sizeof(uint16_t);
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
    const size_t rowBytes = static_cast<size_t>(m_Width) * sizeof(uint16_t);
    memset(m_Band[0], 0, rowBytes * m_BandRows);
    m_Controller.BeginWrite();
    m_Controller.SetWindow(0, 0, m_Width, m_Height);
    // One band of black, sent as often as the screen needs; it never changes,
    // so nothing waits for it to be free.
    for (int32_t y = 0; y < m_Height; y += m_BandRows)
    {
        const int32_t rows = std::min(m_BandRows, m_Height - y);
        m_IO.QueuePixels(m_Band[0], rowBytes * rows, y + rows < m_Height);
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

void PanelDisplay::PushRows(const uint8_t* framebuffer, int width, int height, Deki::ColorFormat format, int y0,
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
    m_Controller.BeginWrite();
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
    }
    m_Controller.EndWrite();
}

void PanelDisplay::PushRowsDirect(const uint16_t* framebuffer, int y0, int y1)
{
    const size_t rowBytes = static_cast<size_t>(m_Width) * sizeof(uint16_t);
    const int chunkRows = static_cast<int>(std::max<size_t>(1, m_IO.GetMaxChunkBytes() / rowBytes));
    m_Controller.BeginWrite();
    m_Controller.SetWindow(0, y0, m_Width, y1);
    for (int y = y0; y < y1; y += chunkRows)
    {
        const int rows = std::min(chunkRows, y1 - y);
        m_IO.QueuePixels(framebuffer + static_cast<size_t>(y) * m_Width, rowBytes * rows, y + rows < y1);
    }
    // Ending the write waits for the last part: the frame is the engine's
    // again when this returns.
    m_Controller.EndWrite();
}

void PanelDisplay::Present(const uint8_t* framebuffer, int width, int height, Deki::ColorFormat format)
{
    if (!m_Initialized || !framebuffer)
    {
        return;
    }
    // Already in the wire's order, the panel's size and with nothing to blend
    // over it: the frame goes out as it is, with no copy.
    const bool direct = format == Deki::ColorFormat::RGB565 && !m_SwapRGB565 && !HasOverlay() &&
                        width == m_Width && height == m_Height;
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
    if (m_RenderBuffers[1])
    {
        m_RenderIndex = 1 - m_RenderIndex;
    }
}

void PanelDisplay::PresentRegions(const uint8_t* framebuffer, int width, int height, Deki::ColorFormat format,
                                  const Deki::Rect* rects, int32_t count)
{
    // Nothing changed on the panel.
    if (!m_Initialized || !framebuffer || count == 0)
    {
        return;
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

    const bool direct = format == Deki::ColorFormat::RGB565 && !m_SwapRGB565 && !HasOverlay() &&
                        width == m_Width && height == m_Height;
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
    m_ActiveOverlay = static_cast<UIOverlay*>(overlay);
    OverlayChanged();
}

void PanelDisplay::ClearActiveUIOverlay()
{
    if (!HasOverlay())
    {
        return;
    }
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
    }
}

bool PanelDisplay::SendCommand(uint8_t cmd, const uint8_t* params, size_t n)
{
    return m_Initialized && m_Controller.SendCommand(cmd, params, n);
}

}  // namespace DekiDisplay
