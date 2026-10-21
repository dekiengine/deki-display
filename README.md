# Deki Display

A display library Deki owns: panel controllers, the buses they are on and (to come) their touch controllers, on any chip a platform package gives an SPI bus for. Deki draws every frame itself, so there is no drawing API here: this is pixels to the panel, and touches back.

`DisplayPanelComponent` starts a panel from the boot scene and gives the engine a `Deki::IDisplay` that presents whole frames or the rectangles of them that changed, with the UI overlay blended over what is sent.

| Controller | Bus | Status |
| --- | --- | --- |
| ST7789 | SPI | runs on the LilyGO T-Deck Plus |

More controllers (ILI9341, ST7789P3, ST7735, GC9A01, SSD1351, RM67162 over QSPI), touch (GT911, FT5x06, CST816S/T, XPT2046) and sending frames from the other core are on their way; until then `deki-lovyangfx-integration` stays every board's default.

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
2. `DisplayPanelComponent` after it: the controller, the SPI host, DC, CS, RST and backlight pins, size, rotation, inversion and colour order.

On a platform with no SPI (the editor, the simulator) the component starts nothing and reports success, so a board's boot scene runs there too.

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
