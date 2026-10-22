# Changelog

Notable changes to `deki-display`. Engine and editor changes are in the
[engine changelog](https://github.com/dekiengine/deki-engine/blob/master/CHANGELOG.md).

A package's `minEngine` names the engine version it needs. Before 1.0 a
breaking change bumps the minor across the editor, the engine and every
package together, so a package with no changes of its own is still released
alongside one that has them.

## Unreleased

### Added
- The package: `DisplayPanelComponent`, which starts a panel on a deki-spi
  bus from the boot scene and gives the engine `PanelDisplay`, a
  `Deki::IDisplay` presenting whole frames and changed rectangles with the UI
  overlay blended over them; `PanelController`, a MIPI DCS controller driven
  by a table per controller (ST7789 first, its start-up and MADCTL ported
  from LovyanGFX); `SpiPanelIO`, 4-wire SPI with pixels queued for DMA; and
  `initSequence`, a board's own start-up for its panel module, as text.
- Async present (`asyncPresent`): each frame is sent from the other core
  while the next is drawn. A whole frame is copied, swapped into the wire's
  byte order as it is copied, to one of two copies in external DMA memory,
  and sent from there by DMA in parts as long as the bus takes, with no bands
  in internal RAM; a strip is copied to one of two copies in internal DMA
  memory and swapped in place a chunk at a time. The worker holds the bus
  across back-to-back frames and strips, with each one's window commands
  queued behind the one before, so the wire does not wait between them; a
  copy goes back to the drawing core as soon as its last part is sent. The
  platform runs the worker through `PanelDisplay::SetWorkerBackend`
  (deki-esp32-integration: a FreeRTOS task pinned to the other core). On a
  LilyGO T-Deck Plus at 80 MHz with the frame rate uncapped: 63.2 fps in
  whole frames with 83.6 KB of internal RAM free, 61.0 in strips of 40 rows
  (LovyanGFX: 62.6 with 73.8 KB, and 60.2).
- Touch: `TouchPanelComponent` starts a GT911 (`GT911Touch`, ported from
  LovyanGFX's driver) on a deki-i2c bus, shared with whatever else is on it,
  and gives deki-input `PanelTouchInput`: a press, moves and a release at
  screen pixels, turned with the display by `TouchMapper` (LovyanGFX's
  default calibration and rotation, so a board's touch bounds carry over).
  Without an INT pin, a point that stops changing is released and not pressed
  again until it moves.
- QSPI panels: `DisplayPanelComponent`'s `busType` `QSPI` drives an AMOLED
  through `QspiPanelIO` (each register write framed 0x02, pixels 0x32 on four
  lines, as LovyanGFX frames them). The RM67162 is the first: its start-up,
  MADCTL and offsets ported from LovyanGFX, rows widened to even, and
  brightness by command where an LCD has a backlight.
- The CST816S/T touch controller (`TouchController::CST816`). It sleeps, and
  does not answer on I2C, until touched: it is set up when it first answers,
  and a read it does not answer is no touch.
- Async present works with render buffers (`usePsram`, `doubleBuffer`) too:
  the worker sends a copy, so the engine never draws into what is being
  sent.
- Tear-free presenting: `DisplayPanelComponent`'s `tePin`, the panel's
  tearing-effect output. A panel refreshes its own rows one after another,
  and a frame written while the refresh passes shows half of each frame.
  With TE on (`0x35`), each frame starts as the panel starts a refresh, and
  goes in the panel's own row order, which a write outruns the refresh in.
  A screen shown turned is turned as it is sent: the controller stays at its
  own orientation, and the renderer is asked for strips of columns in the
  order the panel refreshes them (deki-rendering's `IScanOrderedDisplay`).
  With async present the worker does the turning, a band at a time while
  the band before it is on the wire. Whole frames and strips both. A TE
  line that never pulses is said once in the log, and frames go on without
  waiting. On a LilyGO T-Display S3 AMOLED (its panel refreshes at
  59.6 Hz): 59.5 fps in strips with no tearing, and 28.6 in whole frames,
  which take 25 ms to draw in PSRAM.
