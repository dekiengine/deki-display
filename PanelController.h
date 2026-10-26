#pragma once

#include <cstdint>
#include "IPanelIO.h"
#include "InitSequence.h"

namespace DekiDisplay
{

/// The panel controllers this package drives. Scenes store the name.
///
/// - Over SPI or a parallel bus: ST7789, ST7789P3, ILI9341, ILI9488, ST7796,
///   ST7735 (the B version), ST7735S (the R version), GC9A01 and SSD1351
///   (an OLED, with its own commands).
/// - Over quad SPI only: RM67162 and CO5300, AMOLED controllers.
enum class Controller : uint8_t
{
    ST7789 = 0,
    RM67162 = 1,
    ILI9341 = 2,
    ST7735 = 3,
    ST7735S = 4,
    GC9A01 = 5,
    ST7789P3 = 6,
    ST7796 = 7,
    ILI9488 = 8,
    SSD1351 = 9,
    CO5300 = 10
};

/// The commands a controller takes for what PanelController does. The MIPI
/// DCS controllers (all but the SSD1351) take these defaults.
struct ControllerCommands
{
    uint8_t columns = 0x2A;
    uint8_t rows = 0x2B;
    uint8_t memoryWrite = 0x2C;
    uint8_t madctl = 0x36;
    /// The pixel format command, or 0 where the format is part of MADCTL.
    uint8_t colmod = 0x3A;
    uint8_t invertOff = 0x20;
    uint8_t invertOn = 0x21;
    uint8_t sleepIn = 0x10;
    uint8_t sleepOut = 0x11;
    uint8_t displayOff = 0x28;
    uint8_t displayOn = 0x29;
    uint8_t nop = 0x00;
    /// The software reset, sent at start-up when no reset line is wired, or
    /// 0 for a controller without one.
    uint8_t softwareReset = 0x01;
    /// Bytes per window coordinate: 2 (start and end, high byte first) or 1.
    uint8_t windowBytes = 2;
    /// The column and row commands trade places when the panel is turned a
    /// quarter: the SSD1351 turns its address counter, not its window.
    bool windowTurns = false;

    /// True for the MIPI DCS set: TE on with 0x35, MADCTL's usual bits.
    bool IsMipiDcs() const { return madctl == 0x36; }
};

/// A controller as data: what differs between controllers is the start-up,
/// the MADCTL bits per rotation and a few quirks, more than the commands.
struct ControllerInfo
{
    const char* name = "";
    /// The controller's memory, which a panel module may show only part of.
    uint16_t memoryWidth = 240;
    uint16_t memoryHeight = 320;
    /// The default start-up, in LovyanGFX's table format (InitSequenceFromTable).
    const uint8_t* defaultInit = nullptr;
    /// MADCTL for rotations 0-7 (quarter turns, then the same mirrored),
    /// before the colour-order bit.
    uint8_t madctl[8] = {};
    /// The MADCTL bit for BGR order, set when the panel is not rgbOrder.
    uint8_t bgrBit = 0x08;
    /// A NOP at the end of a write lets a panel with no chip select share its
    /// bus: the NOP ends what the panel would otherwise read as one long
    /// write. Off for a controller that misbehaves on NOP.
    bool nopOnEnd = true;
    /// Writes must start and end on a multiple of this many rows.
    uint8_t rowAlign = 1;
    /// The visible panel's offset in memory turns with the rotation (the
    /// MIPI DCS controllers); false where the offset is applied as it is (the
    /// AMOLED controllers).
    bool offsetsFollowRotation = true;
    /// The command that sets brightness (0x51 on an AMOLED, which has no
    /// backlight), or 0 for none.
    uint8_t brightnessCmd = 0;
    /// The brightness the panel is started at and turned back on to.
    uint8_t onBrightness = 0;
    /// Takes no RGB565 over SPI, only 18 bits a pixel (the ILI9488): pixels
    /// go out as RGB666, three bytes each (Rgb666PanelIO).
    bool spiNeeds18Bits = false;
    /// Turned a quarter, a new column range is only taken after the row range
    /// is opened up (the GC9A01).
    bool openRowsBeforeColumns = false;
    ControllerCommands commands;
};

/// The table entry for `c`; never null.
const ControllerInfo& GetControllerInfo(Controller c);

/// How a panel module is wired to its controller.
struct PanelConfig
{
    Controller controller = Controller::ST7789;
    /// The visible panel, before rotation.
    int32_t panelWidth = 240;
    int32_t panelHeight = 320;
    /// The controller's memory, before rotation. 0 takes the controller's.
    int32_t memoryWidth = 0;
    int32_t memoryHeight = 0;
    /// Where the visible panel starts in the controller's memory.
    int32_t offsetX = 0;
    int32_t offsetY = 0;
    /// 0-3: quarter turns clockwise; 4-7: the same, mirrored.
    uint8_t rotation = 0;
    bool invert = false;
    /// The panel takes red first. Off sets the controller's BGR bit.
    bool rgbOrder = false;
    /// -1 when the reset line is not wired to a GPIO.
    int32_t resetPin = -1;
    /// False for a panel whose chip select is tied low.
    bool hasChipSelect = true;
    /// Pixels go out as RGB666, three bytes each, instead of RGB565.
    bool pixels18 = false;
};

/// Drives one panel controller through an IPanelIO: reset, start-up,
/// rotation, inversion, sleep, and the window pixels are written into. Holds
/// no pixels; PanelDisplay sends them.
///
/// Portions derived from LovyanGFX (https://github.com/lovyan03/LovyanGFX),
/// Copyright (c) lovyan03 and contributors, FreeBSD licence: the rotation and
/// offset arithmetic and the window encoding (Panel_LCD).
class PanelController
{
public:
    using DelayFn = void (*)(uint32_t ms);

    PanelController(IPanelIO& io, DelayFn delay);

    /// Resets the panel, sends `init` (the controller's default when null or
    /// empty), then inversion, the pixel format and the rotation. Leaves the panel on,
    /// showing whatever its memory holds.
    bool Init(const PanelConfig& config, const InitSequence* init);

    bool SetRotation(uint8_t rotation);
    /// Writes in the controller's own orientation (rotation 0) while the
    /// screen stays shown as SetRotation left it: for a display that turns
    /// the pixels before they reach the controller (PanelDisplay's tear-free
    /// presenting). GetScreenRotation keeps the screen's.
    bool WriteInOwnOrientation();
    bool SetInvert(bool invert);
    bool SetSleep(bool sleep);
    bool SetDisplayOn(bool on);
    /// Sets brightness on a controller that has its own (an AMOLED). False on
    /// one that does not: its brightness is a backlight's.
    bool SetBrightness(uint8_t level);
    bool HasBrightness() const { return m_Info->brightnessCmd != 0; }

    /// Holds the bus for a write; EndWrite waits for the pixels and lets it go.
    bool BeginWrite();
    void EndWrite();
    /// Sets the window to [x0, x1) x [y0, y1), in rotated pixels, and opens
    /// the memory write: the pixels that follow fill it row by row.
    bool SetWindow(int32_t x0, int32_t y0, int32_t x1, int32_t y1);

    /// Sends a raw command, for tuning a panel live. Forgets the window, which
    /// the command may have changed.
    bool SendCommand(uint8_t cmd, const uint8_t* params, size_t n);

    /// The panel's size as rotated.
    int32_t GetWidth() const { return m_Width; }
    int32_t GetHeight() const { return m_Height; }
    int32_t GetRowAlign() const { return m_Info->rowAlign; }
    /// How the panel was set up: its size before rotation, and the rotation.
    const PanelConfig& GetConfig() const { return m_Config; }
    uint8_t GetRotation() const { return m_Rotation; }
    /// The rotation the screen is shown in: SetRotation's, also while the
    /// controller is written in its own orientation. What touch turns with.
    uint8_t GetScreenRotation() const { return m_ScreenRotation; }
    const ControllerInfo& GetInfo() const { return *m_Info; }

private:
    bool SendMadctl();
    void ForgetWindow();

    IPanelIO& m_IO;
    DelayFn m_Delay;
    const ControllerInfo* m_Info;
    PanelConfig m_Config;
    uint8_t m_Rotation = 0;
    uint8_t m_ScreenRotation = 0;
    int32_t m_Width = 0;
    int32_t m_Height = 0;
    int32_t m_ColStart = 0;
    int32_t m_RowStart = 0;
    // The window last sent, so an unchanged CASET or RASET is skipped.
    // Columns and rows as the controller counts them, end inclusive.
    int32_t m_Xs = -1;
    int32_t m_Xe = -1;
    int32_t m_Ys = -1;
    int32_t m_Ye = -1;
};

}  // namespace DekiDisplay
