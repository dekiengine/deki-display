# Deki Display

A display library Deki owns: panel controllers, the buses they are on and their touch controllers, on any chip a platform package gives an SPI, an I2C or a parallel bus for. Deki draws every frame itself, so there is no drawing API here: this is pixels to the panel, and touches back.

`DisplayPanelComponent` starts a panel from the boot scene and gives the engine a `Deki::IDisplay` that presents whole frames or the rectangles of them that changed, with the UI overlay blended over what is sent. `TouchPanelComponent` starts its touch controller and gives deki-input a press, moves and a release at screen pixels, turned with the display.

| Part | Bus | Status |
| --- | --- | --- |
| ST7789 panel | SPI, parallel | on SPI, runs on the LilyGO T-Deck Plus |
| RM67162 AMOLED panel | QSPI | runs on the LilyGO T-Display S3 AMOLED |
| ST7789P3, ILI9341, ST7796 panels | SPI, parallel | tested against LovyanGFX's bytes, not yet on a board |
| ILI9488 panel | SPI (18-bit), parallel | tested against LovyanGFX's bytes, not yet on a board |
| ST7735, ST7735S, GC9A01 panels | SPI, parallel | tested against LovyanGFX's bytes, not yet on a board |
| SSD1351 OLED | SPI, parallel | tested against LovyanGFX's bytes, not yet on a board |
| CO5300 AMOLED panel | QSPI | tested against LovyanGFX's bytes, not yet on a board |
| GT911 touch | I2C | runs on the LilyGO T-Deck Plus |
| CST816S/T touch | I2C | runs on the LilyGO T-Display S3 AMOLED |
| FT5x06/FT6x06 touch | I2C | tested against a scripted bus, not yet on a board |
| XPT2046 resistive touch | SPI | tested against recorded readings, not yet on a board |

The start-up tables, MADCTL tables and quirks are ported from LovyanGFX 1.2.29. Parallel means an 8080 bus of 8 or 16 data lines, which the platform drives (deki-esp32-integration: ESP-IDF's esp_lcd i80 driver, on the ESP32-S3's LCD peripheral).

With `asyncPresent` each frame (or strip) is sent from the chip's other core while the next is drawn. On a LilyGO T-Deck Plus at 80 MHz, with the frame rate uncapped, that is 63.2 fps in whole frames (the wire alone allows 65) and 61.0 in strips of 40 rows.

## Tearing

A panel refreshes itself from its own memory, a row at a time, about sixty
times a second. A frame written while the refresh passes shows the top of one
frame and the bottom of another: a line across anything that moves. Shown
turned (landscape on a portrait panel), the panel's rows are the screen's
columns, so a sideways swipe shows it worst.

Wire the panel's tearing-effect output and set `tePin`, and frames never
tear: each starts as the panel starts a refresh, and goes in the panel's own
row order, which the write outruns the refresh in. A turned screen is turned
as it is sent, and drawn in strips of columns. A frame that misses a
refresh's start begins partway into that refresh, once it is far enough
ahead that the write cannot catch it, rather than a whole refresh later.

A board that does not wire TE (the LilyGO T-Deck) can set `scanlineSync`
instead: the panel's scanline is read back (`spiReadHz`, and `readsOnMosi`
for a panel that answers on its data line) to find when each refresh starts.
A write that takes about a refresh, a whole screen on one SPI line, starts
just behind the refresh instead of ahead of it: the refresh it starts in
shows the old frame, and the next shows the new one whole.

Tear-free needs a MIPI DCS controller (not the SSD1351), and a board that
wires its TE output or a panel that can be read back.

## Init sequences belong to the board

A controller's commands are the same on every panel, but the voltages, VCOM and gamma a panel module needs are its own: the LilyGO T-Deck's ST7789 ghosts with the default VCOM. So `initSequence` takes a board's own start-up, as text in the boot scene:

```
11; delay 120          # SLPOUT first
B7 75                  # GCTRL: VGH 14.97 V, VGL -10.43 V
BB 19                  # VCOMS 0.725 V
29; delay 120          # DISPON
```

One command per line or `;`: its hex byte, its parameter bytes, then optionally `delay <ms>`. `#` starts a comment. Empty: the controller's default. After it the panel is always sent its inversion, its pixel format (RGB565, or RGB666 for an ILI9488 on SPI) and its rotation.

## Set-up

1. For a panel on SPI, an SPI Bus component (`deki-spi`) for its bus, first in `PlatformSetupComponent`'s list. A parallel panel needs none: its component starts its bus.
2. `DisplayPanelComponent` after it: the controller, the bus, the CS, RST and backlight pins, size, rotation, inversion and colour order. The bus is `SPI` (the SPI host and a DC pin), `QSPI` (an AMOLED on an SPI bus with IO2 and IO3), or `Parallel8` / `Parallel16` (the DC, WR and RD pins, D0 to D7 or D15, and the write clock). An AMOLED has no backlight: its brightness is a command, sent when the engine turns the screen off and on.
3. For touch, its bus's component, then `TouchPanelComponent` after the display. GT911, CST816 and FT5x06 are on I2C (`deki-i2c`): the I2C port, INT and RST pins. XPT2046 is on SPI, often the display's bus: the SPI host, CS and its PENIRQ as the INT pin. Then the raw bounds along the panel's width and height before rotation; an XPT2046's need calibrating (it reads about 300 to 3900).

On a platform with none of these buses (the editor, the simulator) the components start nothing and report success, so a board's boot scene runs there too.

What the device decides for itself (frames sent from the other core, tear-free frames from the TE line or the scanline) and what a board must set is in the engine's guide, [Making it fast](https://github.com/dekiengine/deki-editor/blob/master/docs/performance.md).

## Namespace

Types live in `DekiDisplay`. Scene files store the qualified name, and so does code:

```cpp
using namespace DekiDisplay;
obj->AddComponent<DisplayPanelComponent>();
```

## Install

Package Manager in the Deki Editor, or `DekiEditor --packages-add deki-display <project>`.

## License

Apache 2.0. See [LICENSE](LICENSE). Parts are derived from [LovyanGFX](https://github.com/lovyan03/LovyanGFX) under its FreeBSD licence: see [NOTICE](NOTICE).
