# Deki Display

A display library Deki owns: panel controllers, the buses they are on and their touch controllers, on any chip a platform package gives an SPI and an I2C bus for. Deki draws every frame itself, so there is no drawing API here: this is pixels to the panel, and touches back.

`DisplayPanelComponent` starts a panel from the boot scene and gives the engine a `Deki::IDisplay` that presents whole frames or the rectangles of them that changed, with the UI overlay blended over what is sent. `TouchPanelComponent` starts its touch controller and gives deki-input a press, moves and a release at screen pixels, turned with the display.

| Part | Bus | Status |
| --- | --- | --- |
| ST7789 panel | SPI | runs on the LilyGO T-Deck Plus |
| RM67162 AMOLED panel | QSPI | runs on the LilyGO T-Display S3 AMOLED |
| GT911 touch | I2C | runs on the LilyGO T-Deck Plus |
| CST816S/T touch | I2C | runs on the LilyGO T-Display S3 AMOLED |

With `asyncPresent` each frame (or strip) is sent from the chip's other core while the next is drawn. On a LilyGO T-Deck Plus at 80 MHz, with the frame rate uncapped, that is 63.2 fps in whole frames (the wire alone allows 65) and 61.0 in strips of 40 rows.

More controllers (ILI9341, ST7789P3, ST7735, GC9A01, SSD1351) and touch (FT5x06, XPT2046) are on their way; until then `deki-lovyangfx-integration` stays available for them.

## Init sequences belong to the board

A controller's commands are the same on every panel, but the voltages, VCOM and gamma a panel module needs are its own: the LilyGO T-Deck's ST7789 ghosts with the default VCOM. So `initSequence` takes a board's own start-up, as text in the boot scene:

```
11; delay 120          # SLPOUT first
B7 75                  # GCTRL: VGH 14.97 V, VGL -10.43 V
BB 19                  # VCOMS 0.725 V
29; delay 120          # DISPON
```

One command per line or `;`: its hex byte, its parameter bytes, then optionally `delay <ms>`. `#` starts a comment. Empty: the controller's default. After it the panel is always sent its inversion, RGB565 and its rotation.

## Set-up

1. An SPI Bus component (`deki-spi`) for the panel's bus, first in `PlatformSetupComponent`'s list.
2. `DisplayPanelComponent` after it: the controller, the bus (`SPI` with a DC pin, or `QSPI` for an AMOLED on a bus with IO2 and IO3), the SPI host, CS, RST and backlight pins, size, rotation, inversion and colour order. An AMOLED has no backlight: its brightness is a command, sent when the engine turns the screen off and on.
3. For touch, an I2C Bus component (`deki-i2c`) for the controller's bus, then `TouchPanelComponent` after the display: the controller, the I2C port, INT and RST pins, and the raw bounds along the panel's width and height before rotation.

On a platform with no SPI or I2C (the editor, the simulator) the components start nothing and report success, so a board's boot scene runs there too.

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
