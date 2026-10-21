#pragma once

#include <cstdint>
#include <string>
#include <deki/SetupComponent.h>
#include <deki/reflection/Property.h>
#include "DekiDisplayPackage.h"
#include "PanelController.h"

namespace DekiDisplay
{

enum class DisplayRotation : uint8_t
{
    Portrait = 0,
    Landscape = 1,
    Portrait180 = 2,
    Landscape270 = 3,
    PortraitMirror = 4,
    LandscapeMirror = 5,
    Portrait180Mirror = 6,
    Landscape270Mirror = 7
};

/// Starts a display panel from the boot scene and gives it to the engine:
/// the controller, how it is wired, and the start-up its panel module needs.
///
/// The panel is on an SPI bus an SPI Bus component (deki-spi) has started,
/// before this one in PlatformSetupComponent's list. Touch comes after it.
///
/// On a platform with no SPI (the editor, the simulator) it reports success
/// and starts nothing, so a board's boot scene runs there too.
DEKI_CATEGORY("Display")
DEKI_DESCRIPTION("Drives a display panel (ST7789, ...) over SPI with Deki's own display library, and gives it to "
                 "the engine.")
class DEKI_DISPLAY_API DisplayPanelComponent : public Deki::SetupComponent
{
public:
    // ========== Panel ==========

    DEKI_EXPORT
    DEKI_TOOLTIP("The panel's controller IC")
    Controller controller = Controller::ST7789;

    DEKI_EXPORT
    DEKI_TOOLTIP("The commands that start this panel module, replacing the controller's default. A panel's voltages, "
                 "VCOM and gamma belong to the module, not the controller, so a board sets them here. One command per "
                 "line or ';': its hex byte, then its parameter bytes, then optionally 'delay <ms>'. '#' starts a "
                 "comment. Empty: the controller's default")
    std::string initSequence;

    DEKI_EXPORT
    DEKI_TOOLTIP("Visible width in pixels, before rotation")
    DEKI_RANGE(1, 1024)
    int32_t panelWidth = 240;

    DEKI_EXPORT
    DEKI_TOOLTIP("Visible height in pixels, before rotation")
    DEKI_RANGE(1, 1024)
    int32_t panelHeight = 320;

    DEKI_EXPORT
    DEKI_TOOLTIP("How the panel is turned")
    DisplayRotation rotation = DisplayRotation::Landscape;

    DEKI_EXPORT
    DEKI_TOOLTIP("Where the visible panel starts in the controller's memory, across (for panels with a non-zero "
                 "origin)")
    int32_t offsetX = 0;

    DEKI_EXPORT
    DEKI_TOOLTIP("Where the visible panel starts in the controller's memory, down")
    int32_t offsetY = 0;

    DEKI_EXPORT
    DEKI_TOOLTIP("Invert the panel's colours (most IPS panels need it on)")
    bool invertColor = false;

    DEKI_EXPORT
    DEKI_TOOLTIP("The panel takes red first. Off sets the controller's BGR order")
    bool rgbOrder = false;

    // ========== Bus ==========

    DEKI_GROUP("Bus")
    DEKI_EXPORT
    DEKI_TOOLTIP("The SPI host the panel is on, as the SPI Bus component that starts it numbers it (on an ESP32-S3: "
                 "1 = SPI2, 2 = SPI3)")
    DEKI_RANGE(0, 3)
    int32_t spiHost = 1;

    DEKI_EXPORT
    DEKI_UNIT(Frequency)
    DEKI_TOOLTIP("SPI clock for the panel. 80 MHz is the ESP32-S3's fastest, and most ST7789 modules take it")
    int32_t spiWriteHz = 40000000;

    DEKI_EXPORT
    DEKI_TOOLTIP("Data/command pin (DC)")
    DEKI_RANGE(-1, 48)
    int32_t dcPin = -1;

    // ========== Control pins ==========

    DEKI_GROUP("Control Pins")
    DEKI_EXPORT
    DEKI_TOOLTIP("Chip select pin (-1 = tied low)")
    DEKI_RANGE(-1, 48)
    int32_t csPin = -1;

    DEKI_EXPORT
    DEKI_TOOLTIP("Reset pin (-1 = not connected)")
    DEKI_RANGE(-1, 48)
    int32_t rstPin = -1;

    // ========== Backlight ==========

    DEKI_GROUP("Backlight")
    DEKI_EXPORT
    DEKI_TOOLTIP("Backlight pin (-1 = none). Held high for on: a backlight driver dimmed by counting pulses (the "
                 "LilyGO T-Deck's) wants exactly that")
    DEKI_RANGE(-1, 48)
    int32_t blPin = -1;

    DEKI_EXPORT
    DEKI_TOOLTIP("The backlight is on when its pin is low")
    bool blInvert = false;

    // ========== Advanced ==========

    DEKI_GROUP("Advanced")
    DEKI_EXPORT
    DEKI_TOOLTIP("The controller's memory width, which may be more than the visible width (0 = the controller's)")
    DEKI_RANGE(0, 1024)
    int32_t memoryWidth = 0;

    DEKI_EXPORT
    DEKI_TOOLTIP("The controller's memory height (0 = the controller's)")
    DEKI_RANGE(0, 1024)
    int32_t memoryHeight = 0;

    DEKI_EXPORT
    DEKI_TOOLTIP("Keep the framebuffer in PSRAM instead of internal RAM: frees a screen's worth of internal RAM, "
                 "drawing is slower")
    bool usePsram = false;

    DEKI_EXPORT
    DEKI_TOOLTIP("Two framebuffers, swapped after each frame")
    bool doubleBuffer = false;

    DEKI_EXPORT
    DEKI_TOOLTIP("Send each frame from the chip's other core while the next is drawn: a frame costs the longer of "
                 "drawing and sending instead of both. Takes two frames of external memory (PSRAM) and a dual-core "
                 "chip")
    bool asyncPresent = false;

    // ========== SetupComponent ==========

    void Setup(SetupCallback onComplete) override;
    const char* GetSetupName() const override { return "Display Panel"; }

    /// The started panel's controller, for touch to turn with it; nullptr
    /// before Setup has started one.
    static const PanelController* GetStartedController();
};

}  // namespace DekiDisplay
