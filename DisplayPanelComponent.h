#pragma once

#include <cstdint>
#include <string>
#include <deki/SetupComponent.h>
#include <deki/reflection/Property.h>
#include "DekiDisplayPackage.h"
#include "PanelController.h"

namespace DekiDisplay
{

enum class DisplayBus : uint8_t
{
    SPI = 0,
    QSPI = 1,
    Parallel8 = 2,
    Parallel16 = 3
};

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
/// before this one in PlatformSetupComponent's list, or on its own parallel
/// bus, which this starts. Touch comes after it.
///
/// On a platform with no such bus (the editor, the simulator) it reports
/// success and starts nothing, so a board's boot scene runs there too.
DEKI_CATEGORY("Display")
DEKI_DESCRIPTION("Drives a display panel (ST7789, ILI9341, GC9A01, ...) over SPI, quad SPI or an 8- or 16-bit "
                 "parallel bus with Deki's own display library, and gives it to the engine.")
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

    DEKI_EXPORT
    DEKI_TOOLTIP("Sends each pixel's two bytes the other way round. Turn it on when dark colours come out pink and "
                 "white comes out green. Costs a copy of every frame")
    bool swapBytes = false;

    // ========== Bus ==========

    DEKI_GROUP("Bus")
    DEKI_EXPORT
    DEKI_TOOLTIP("How the panel is wired: SPI with a data/command pin (most LCDs), quad SPI (AMOLED panels such as "
                 "the RM67162), which needs an SPI Bus with IO2 and IO3, or an 8- or 16-bit parallel bus (8080), "
                 "which this component starts itself")
    DisplayBus busType = DisplayBus::SPI;

    DEKI_EXPORT
    DEKI_TOOLTIP("The SPI host the panel is on, as the SPI Bus component that starts it numbers it (on an ESP32-S3: "
                 "1 = SPI2, 2 = SPI3)")
    DEKI_VISIBLE_WHEN(busType, SPI, QSPI)
    DEKI_RANGE(0, 3)
    int32_t spiHost = 1;

    DEKI_EXPORT
    DEKI_UNIT(Frequency)
    DEKI_TOOLTIP("SPI clock for the panel. 80 MHz is the ESP32-S3's fastest, and most ST7789 modules take it")
    DEKI_VISIBLE_WHEN(busType, SPI, QSPI)
    int32_t spiWriteHz = 40000000;

    DEKI_EXPORT
    DEKI_UNIT(Frequency)
    DEKI_TOOLTIP("Write strobe frequency on the parallel bus. Most panels take 20 MHz; some, with short wires, more")
    DEKI_VISIBLE_WHEN(busType, Parallel8, Parallel16)
    int32_t parallelWriteHz = 20000000;

    DEKI_EXPORT
    DEKI_TOOLTIP("Data/command pin (DC, also called RS)")
    DEKI_VISIBLE_WHEN(busType, SPI, Parallel8, Parallel16)
    DEKI_RANGE(-1, 48)
    int32_t dcPin = -1;

    DEKI_EXPORT
    DEKI_TOOLTIP("Write strobe pin (WR)")
    DEKI_VISIBLE_WHEN(busType, Parallel8, Parallel16)
    DEKI_RANGE(-1, 48)
    int32_t wrPin = -1;

    DEKI_EXPORT
    DEKI_TOOLTIP("Read strobe pin (RD), held high: the panel is only written (-1 = tied high on the board)")
    DEKI_VISIBLE_WHEN(busType, Parallel8, Parallel16)
    DEKI_RANGE(-1, 48)
    int32_t rdPin = -1;

    DEKI_EXPORT
    DEKI_TOOLTIP("Data line D0")
    DEKI_VISIBLE_WHEN(busType, Parallel8, Parallel16)
    DEKI_RANGE(-1, 48)
    int32_t d0Pin = -1;

    DEKI_EXPORT
    DEKI_TOOLTIP("Data line D1")
    DEKI_VISIBLE_WHEN(busType, Parallel8, Parallel16)
    DEKI_RANGE(-1, 48)
    int32_t d1Pin = -1;

    DEKI_EXPORT
    DEKI_TOOLTIP("Data line D2")
    DEKI_VISIBLE_WHEN(busType, Parallel8, Parallel16)
    DEKI_RANGE(-1, 48)
    int32_t d2Pin = -1;

    DEKI_EXPORT
    DEKI_TOOLTIP("Data line D3")
    DEKI_VISIBLE_WHEN(busType, Parallel8, Parallel16)
    DEKI_RANGE(-1, 48)
    int32_t d3Pin = -1;

    DEKI_EXPORT
    DEKI_TOOLTIP("Data line D4")
    DEKI_VISIBLE_WHEN(busType, Parallel8, Parallel16)
    DEKI_RANGE(-1, 48)
    int32_t d4Pin = -1;

    DEKI_EXPORT
    DEKI_TOOLTIP("Data line D5")
    DEKI_VISIBLE_WHEN(busType, Parallel8, Parallel16)
    DEKI_RANGE(-1, 48)
    int32_t d5Pin = -1;

    DEKI_EXPORT
    DEKI_TOOLTIP("Data line D6")
    DEKI_VISIBLE_WHEN(busType, Parallel8, Parallel16)
    DEKI_RANGE(-1, 48)
    int32_t d6Pin = -1;

    DEKI_EXPORT
    DEKI_TOOLTIP("Data line D7")
    DEKI_VISIBLE_WHEN(busType, Parallel8, Parallel16)
    DEKI_RANGE(-1, 48)
    int32_t d7Pin = -1;

    DEKI_EXPORT
    DEKI_TOOLTIP("Data line D8")
    DEKI_VISIBLE_WHEN(busType, Parallel16)
    DEKI_RANGE(-1, 48)
    int32_t d8Pin = -1;

    DEKI_EXPORT
    DEKI_TOOLTIP("Data line D9")
    DEKI_VISIBLE_WHEN(busType, Parallel16)
    DEKI_RANGE(-1, 48)
    int32_t d9Pin = -1;

    DEKI_EXPORT
    DEKI_TOOLTIP("Data line D10")
    DEKI_VISIBLE_WHEN(busType, Parallel16)
    DEKI_RANGE(-1, 48)
    int32_t d10Pin = -1;

    DEKI_EXPORT
    DEKI_TOOLTIP("Data line D11")
    DEKI_VISIBLE_WHEN(busType, Parallel16)
    DEKI_RANGE(-1, 48)
    int32_t d11Pin = -1;

    DEKI_EXPORT
    DEKI_TOOLTIP("Data line D12")
    DEKI_VISIBLE_WHEN(busType, Parallel16)
    DEKI_RANGE(-1, 48)
    int32_t d12Pin = -1;

    DEKI_EXPORT
    DEKI_TOOLTIP("Data line D13")
    DEKI_VISIBLE_WHEN(busType, Parallel16)
    DEKI_RANGE(-1, 48)
    int32_t d13Pin = -1;

    DEKI_EXPORT
    DEKI_TOOLTIP("Data line D14")
    DEKI_VISIBLE_WHEN(busType, Parallel16)
    DEKI_RANGE(-1, 48)
    int32_t d14Pin = -1;

    DEKI_EXPORT
    DEKI_TOOLTIP("Data line D15")
    DEKI_VISIBLE_WHEN(busType, Parallel16)
    DEKI_RANGE(-1, 48)
    int32_t d15Pin = -1;

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

    DEKI_EXPORT
    DEKI_TOOLTIP("The panel's tearing-effect (TE) output (-1 = not connected). With it, frames never tear: each "
                 "starts as the panel starts a refresh, and goes in the panel's own row order, a turned screen "
                 "turned as it is sent and drawn in strips of columns")
    DEKI_RANGE(-1, 48)
    int32_t tePin = -1;

    DEKI_EXPORT
    DEKI_TOOLTIP("With no TE line wired, read the panel's scanline instead, so frames still never tear. A panel "
                 "that cannot be read back is sent as frames are drawn, and the boot log says so")
    DEKI_VISIBLE_WHEN(busType, SPI)
    bool scanlineSync = true;

    DEKI_EXPORT
    DEKI_UNIT(Frequency)
    DEKI_TOOLTIP("SPI clock for reading the panel back, slower than writing it (the ST7789 reads reliably at 10 MHz)")
    DEKI_VISIBLE_WHEN(scanlineSync, 1)
    int32_t spiReadHz = 10000000;

    DEKI_EXPORT
    DEKI_TOOLTIP("The panel answers on its data-in line (SDA both ways, as the LilyGO T-Deck's does), not on MISO")
    DEKI_VISIBLE_WHEN(scanlineSync, 1)
    bool readsOnMosi = false;

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
                 "drawing and sending instead of both. Needs a dual-core chip and two frames of external memory "
                 "(PSRAM); without them, frames are sent as they are drawn, and the boot log says so. The sender "
                 "runs on the core the game does not, at priority 5: about 1% of that core when little moves, and up "
                 "to 20 to 40% when most of the screen changes (measured on ESP32-S3 boards). A task of your own on "
                 "that core above priority 5 runs first. Off, the whole core is the game's, and each frame costs "
                 "drawing and sending.")
    bool asyncPresent = true;

    // ========== SetupComponent ==========

    void Setup(SetupCallback onComplete) override;
    const char* GetSetupName() const override { return "Display Panel"; }

    /// The started panel's controller, for touch to turn with it; nullptr
    /// before Setup has started one.
    static const PanelController* GetStartedController();
};

}  // namespace DekiDisplay
