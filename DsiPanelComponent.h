#pragma once

#include <cstdint>
#include <string>
#include <deki/SetupComponent.h>
#include <deki/reflection/Property.h>
#include "DekiDisplayPackage.h"

namespace DekiDisplay
{

/// Starts a MIPI-DSI panel from the boot scene and gives it to the engine:
/// its link, the commands that start its controller, and the timing of the
/// frames streamed to it.
///
/// The panel runs in video mode: the chip streams it whole frames from two
/// frame buffers in its memory, and the game draws straight into the one
/// not on screen (DsiDisplay). Its controller is started with `initSequence`
/// over the DSI link, as the panel's maker gives it.
///
/// On a platform with no DSI (the editor, the simulator, a chip without
/// one) it reports success and starts nothing, so a board's boot scene runs
/// there too.
DEKI_CATEGORY("Display")
DEKI_DESCRIPTION("Drives a MIPI-DSI panel in video mode (EK79007, ...) from frame buffers the game draws into "
                 "directly, and gives it to the engine.")
class DEKI_DISPLAY_API DsiPanelComponent : public Deki::SetupComponent
{
public:
    // ========== Panel ==========

    DEKI_EXPORT
    DEKI_TOOLTIP("The commands that start the panel's controller, sent over the DSI link before the video stream "
                 "starts. They come from the panel's maker. One command per line or ';': its hex byte, then its "
                 "parameter bytes, then optionally 'delay <ms>'. '#' starts a comment")
    std::string initSequence;

    DEKI_EXPORT
    DEKI_TOOLTIP("Width in pixels")
    DEKI_RANGE(1, 4096)
    int32_t panelWidth = 1024;

    DEKI_EXPORT
    DEKI_TOOLTIP("Height in pixels")
    DEKI_RANGE(1, 4096)
    int32_t panelHeight = 600;

    DEKI_EXPORT
    DEKI_TOOLTIP("Frame buffers. Two: one is drawn while the other is shown, so nothing tears, but each frame waits "
                 "for the refresh still sending the one before. Three: there is always one free, so drawing never "
                 "waits, for a frame's memory more. One saves a frame's memory, and frames are drawn while they are "
                 "shown")
    DEKI_RANGE(1, 3)
    int32_t frameBuffers = 2;

    // ========== Link ==========

    DEKI_GROUP("Link")
    DEKI_EXPORT
    DEKI_TOOLTIP("Data lanes the panel is wired with")
    DEKI_RANGE(1, 4)
    int32_t dataLanes = 2;

    DEKI_EXPORT
    DEKI_TOOLTIP("Bit rate of each data lane, in Mbps, as the panel's maker gives it")
    DEKI_RANGE(80, 2500)
    int32_t laneMbps = 500;

    DEKI_EXPORT
    DEKI_UNIT(Frequency)
    DEKI_TOOLTIP("Pixel clock of the video stream. With the porches, it sets the refresh rate")
    int32_t pixelClockHz = 0;

    // ========== Timing ==========

    DEKI_GROUP("Timing")
    DEKI_EXPORT
    DEKI_TOOLTIP("Horizontal sync pulse, in pixels")
    DEKI_RANGE(0, 1024)
    int32_t hsyncPulse = 0;

    DEKI_EXPORT
    DEKI_TOOLTIP("Horizontal back porch, in pixels")
    DEKI_RANGE(0, 1024)
    int32_t hBackPorch = 0;

    DEKI_EXPORT
    DEKI_TOOLTIP("Horizontal front porch, in pixels")
    DEKI_RANGE(0, 1024)
    int32_t hFrontPorch = 0;

    DEKI_EXPORT
    DEKI_TOOLTIP("Vertical sync pulse, in lines")
    DEKI_RANGE(0, 1024)
    int32_t vsyncPulse = 0;

    DEKI_EXPORT
    DEKI_TOOLTIP("Vertical back porch, in lines")
    DEKI_RANGE(0, 1024)
    int32_t vBackPorch = 0;

    DEKI_EXPORT
    DEKI_TOOLTIP("Vertical front porch, in lines")
    DEKI_RANGE(0, 1024)
    int32_t vFrontPorch = 0;

    // ========== Control pins ==========

    DEKI_GROUP("Control pins")
    DEKI_EXPORT
    DEKI_TOOLTIP("The panel's reset pin, pulled low and released before its start-up (-1: not wired, the start-up "
                 "begins with a software reset instead)")
    int32_t rstPin = -1;

    DEKI_EXPORT
    DEKI_TOOLTIP("Backlight enable (-1 = none)")
    int32_t blPin = -1;

    DEKI_EXPORT
    DEKI_TOOLTIP("The backlight is on when its pin is low")
    bool blInvert = false;

    DEKI_EXPORT
    DEKI_UNIT(Frequency)
    DEKI_TOOLTIP("Drive the backlight pin with PWM at this frequency, for brightness and for backlight drivers made for "
                 "PWM, which can flicker with the pin held high. 0: the pin is switched on and off")
    int32_t blPwmHz = 0;

    DEKI_EXPORT
    DEKI_TOOLTIP("The backlight's brightness, in percent, with PWM")
    DEKI_RANGE(1, 100)
    int32_t blBrightness = 100;

    // ========== Drawing ==========

    DEKI_GROUP("Drawing")
    DEKI_EXPORT
    DEKI_TOOLTIP("Take frames in strips drawn in internal RAM, each copied into the frame buffer (by DMA where the "
                 "platform has it) while the next is drawn: for frame buffers in PSRAM, which internal RAM is much "
                 "faster than. Off: frames are drawn straight into the frame buffers. The project's Frame Mode (Auto "
                 "or Strips) and its strip height still decide")
    bool drawInStrips = false;

    void Setup(SetupCallback onComplete) override;
    const char* GetSetupName() const override { return "DSI Panel"; }

    /// Stops the started panel, if there is one, and takes its display back
    /// from the engine, so a panel can be started again.
    static void Stop();

    /// The started panel's size, for touch laid over it. False when no DSI
    /// panel is started.
    static bool GetStartedSize(int32_t& width, int32_t& height);
};

}  // namespace DekiDisplay
