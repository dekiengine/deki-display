#pragma once

#include "DekiDisplayPackage.h"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace DekiDisplay
{

/// How a MIPI-DSI panel in video mode is driven: its size, its DSI link, and
/// the timing of the frames streamed to it.
///
/// A video-mode panel has no memory of its own to write a window into: the
/// chip streams it whole frames, continuously, from frame buffers it holds.
struct DsiPanelConfig
{
    int32_t width = 0;
    int32_t height = 0;

    /// Data lanes the panel is wired with (1, 2 or 4).
    uint8_t dataLanes = 2;
    /// Bit rate of each lane.
    uint32_t laneMbps = 500;
    /// Pixel clock of the video stream.
    uint32_t pixelClockHz = 0;

    // Video timing, in pixels across and lines down.
    uint16_t hsyncPulse = 0;
    uint16_t hBackPorch = 0;
    uint16_t hFrontPorch = 0;
    uint16_t vsyncPulse = 0;
    uint16_t vBackPorch = 0;
    uint16_t vFrontPorch = 0;

    /// Frame buffers, 1 to 3. With 2 a frame is drawn while the other is
    /// shown, but the next waits for the refresh still sending the one before
    /// it. With 3 there is always one the panel is not reading, so drawing
    /// never waits. 1 saves the memory, and frames tear.
    uint8_t frameBuffers = 2;

    /// The time one refresh takes at this timing, 0 when it is not set.
    uint32_t RefreshPeriodUs() const
    {
        if (pixelClockHz == 0)
        {
            return 0;
        }
        const uint64_t across = static_cast<uint64_t>(width) + hsyncPulse + hBackPorch + hFrontPorch;
        const uint64_t down = static_cast<uint64_t>(height) + vsyncPulse + vBackPorch + vFrontPorch;
        return static_cast<uint32_t>(across * down * 1000000ull / pixelClockHz);
    }
};

/// A MIPI-DSI panel as the platform drives it: commands over the DSI link,
/// then a video stream from frame buffers the platform holds, in RGB565.
class IDsiPanel
{
public:
    virtual ~IDsiPanel() = default;

    /// Sends a DCS command and its `n` parameter bytes over the link. The
    /// panel's start-up goes this way, before Start.
    virtual bool Command(uint8_t cmd, const uint8_t* params, size_t n) = 0;

    /// Starts the video stream, from frame buffer 0.
    virtual bool Start() = 0;

    virtual int32_t GetFrameBufferCount() const = 0;
    /// Frame buffer `index`, width * height RGB565 pixels.
    virtual uint8_t* GetFrameBuffer(int32_t index) const = 0;

    /// Shows frame buffer `index` from the next refresh on, once what the
    /// CPU wrote to it has reached memory. Returns at once. A buffer shown
    /// and replaced before a refresh began is never sent.
    virtual bool Show(int32_t index) = 0;

    /// The buffers the panel may still read, one bit each: the one being sent
    /// now and the one shown last, which the next refresh sends. Any other
    /// may be drawn into.
    virtual uint32_t GetBusyMask() const = 0;

    /// Waits for the refresh being sent to end, up to `timeoutMs`; false when
    /// none ended in time.
    virtual bool WaitRefresh(uint32_t timeoutMs) = 0;

    /// How many Shows have gone up (their buffer started being sent to the
    /// panel) since Start, and when the latest did (microseconds on the
    /// platform's clock).
    virtual bool GetFramesShown(uint64_t* frames, uint64_t* lastUs) const = 0;

    /// Drives the backlight on `pin` with PWM at `hz`, off for now. False
    /// when the platform cannot; the display then switches the pin on and
    /// off.
    virtual bool StartBacklightPwm(int32_t pin, uint32_t hz, bool invert)
    {
        (void)pin;
        (void)hz;
        (void)invert;
        return false;
    }
    /// The PWM backlight's brightness, 0 (off) to 1 (always on).
    virtual bool SetBacklightLevel(float level)
    {
        (void)level;
        return false;
    }

    /// Starts copying `rows` whole rows of RGB565 (`srcStridePixels` apart)
    /// into frame buffer `index` from row `y`, going on until WaitRowsCopy;
    /// one at a time, so a second waits for the first. False, having started
    /// nothing, when the platform cannot: the caller copies them itself.
    virtual bool StartRowsCopy(int32_t index, int32_t y, int32_t rows, const uint8_t* src, int32_t srcStridePixels)
    {
        (void)index;
        (void)y;
        (void)rows;
        (void)src;
        (void)srcStridePixels;
        return false;
    }
    /// Waits for the copy StartRowsCopy started, if one is going.
    virtual void WaitRowsCopy() {}
    /// Copies whole rows [y, y + rows) of frame buffer `from` into frame
    /// buffer `to`, and returns once they are there. False when the platform
    /// cannot: the caller copies them itself.
    virtual bool CopyRowsBetween(int32_t from, int32_t to, int32_t y, int32_t rows)
    {
        (void)from;
        (void)to;
        (void)y;
        (void)rows;
        return false;
    }
};

/// Makes MIPI-DSI panels: the platform package registers a factory, and
/// DsiPanelComponent asks it for the panel the board has.
class DEKI_DISPLAY_API MipiDsi
{
public:
    /// The panel for `config`, or nullptr when it cannot be started (the
    /// factory logs why).
    using Factory = std::unique_ptr<IDsiPanel> (*)(const DsiPanelConfig& config);

    static void SetFactory(Factory factory);
    static bool HasFactory();
    /// The factory's panel for `config`; nullptr without a factory.
    static std::unique_ptr<IDsiPanel> Create(const DsiPanelConfig& config);

private:
    static Factory s_Factory;
};

}  // namespace DekiDisplay
