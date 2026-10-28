# Changelog

Notable changes to `deki-display`. Engine and editor changes are in the
[engine changelog](https://github.com/dekiengine/deki-engine/blob/master/CHANGELOG.md).

A package's `minEngine` names the engine version it needs. Before 1.0 a
breaking change bumps the minor across the editor, the engine and every
package together, so a package with no changes of its own is still released
alongside one that has them.

## 0.21.0

### Added
- **Swap Bytes** on `DisplayPanelComponent` (`PanelDisplay::Options::swapBytes`):
  each pixel's two bytes go out the other way round, for a panel that reads
  them so. The symptom is dark colours turning pink and white turning green.
  Frames are then swapped in a copy, which costs a little time per frame; off,
  nothing changes.

### Changed
- `minEngine` 0.21.0: built against engine 0.21.0. A build of this package
  from before does not load and is rebuilt.
- A frame where several things moved is sent from the other core too, as a
  whole frame or one changed rectangle already was: the changed rows go into
  a frame copy and the drawing core draws on. They were sent as drawn, which
  kept the drawing core busy for as long as the wire took. On a T-Deck Plus
  the demo's effects scene presents in 3.5 ms instead of 4.3 ms.
- A panel that shows each frame on its refresh (a TE pin or its scanline)
  tells the engine how long a refresh takes, and the engine times frames to
  it. On a T-Deck Plus, whose panel refreshes at 60.2 Hz, a game at 60
  frames a second showed one frame twice every five seconds, a small
  stutter in anything scrolling; it no longer does. The frame readout's
  present time now includes the wait for the panel.

### Fixed
- A panel with no reset line starts from its own defaults. Restarting the
  chip does not reset the panel, so it kept whatever the firmware before had
  set, such as a byte order or gamma no part of this start-up sets: a T-Deck
  that had run firmware leaving its panel little-endian showed dark colours
  pink. Without a reset pin the controller now gets a software reset (0x01)
  first, then 120 ms, as ESP-IDF's driver does; not twice where the start-up
  begins with one, and not on the SSD1351, which has none. Boot takes 120 ms
  longer on such boards.

## 0.20.0

### Changed
- `minEngine` 0.20.0: built against engine 0.20.0. A build of this package from before does not load and is rebuilt.

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
- More controllers, each a table ported from LovyanGFX 1.2.29: ILI9341,
  ST7735 (the B version), ST7735S (the R version), GC9A01, ST7789P3, ST7796,
  ILI9488 and SSD1351 on SPI or a parallel bus, and the CO5300 AMOLED on
  QSPI. A controller names its own commands (`ControllerCommands`): the
  SSD1351's window is a byte a coordinate, its column and row commands trade
  places when turned, and its remap takes MADCTL's place. The GC9A01 opens
  its rows before a turned window's columns, as LovyanGFX does. Checked
  against LovyanGFX's bytes in tests; none has run on a board yet.
- 18 bits a pixel: the ILI9488 takes no RGB565 over SPI, so its pixels go out
  as RGB666 through `Rgb666PanelIO`, which widens each piece into its own DMA
  buffers while the piece before is sent, and COLMOD is 0x66.
- Parallel panels: `busType` `Parallel8` and `Parallel16`, an 8080 bus of 8
  or 16 data lines with DC, WR and RD pins and its write clock
  (`parallelWriteHz`). The platform drives the bus (`ParallelBus`, a factory
  it registers; `ParallelPanelIO` speaks to it), and the component starts it
  itself. On 16 lines a pixel goes as memory holds it, so a little-endian
  frame needs no byte swap.
- Touch: the FT5x06 family (FT5206, FT6206, FT6336 and others) on I2C, and
  the XPT2046 resistive controller on SPI, both ported from LovyanGFX. The
  XPT2046's raw readings need calibrating in the touch bounds. A controller
  whose count or pressure says when the finger has gone
  (`ITouchController::MayRepeatLastTouch` false) is not released for holding
  still.
- Tear-free: a frame that missed its refresh's start is started for the
  longest write any frame has taken (`PanelDisplay::LongestWriteUs`), not the
  shorter of the last two. The frame that opens a new scene is late (the
  scene loads in it) and changes the whole screen, while the frames before it
  changed a little: started for theirs, it began too late, the refresh caught
  it, and the screen tore across.
- Tear-free with no TE line: `scanlineSync` reads the panel's scanline
  (Get_Scanline, 0x45) back through the panel IO (`IPanelIO::Read`) at its
  own clock (`spiReadHz`), on MISO or, with `readsOnMosi`, on the panel's
  data line. `ScanlineClock` finds two refresh starts, where the count drops
  from the last row to the blanking (it holds there; the low byte rolling
  over at row 256 does not), and keeps the clock right with each start it
  catches. On a LilyGO T-Deck Plus: 60.2 Hz found, every demo scene at 60
  fps with frames in step with the refresh.
- A frame that takes about a refresh to go out, or up to two, is started
  just behind the refresh rather than at once (`PanelDisplay::WritesBehind`):
  it never catches the refresh's rows up, and the next refresh shows it
  whole. Before, such a frame was taken to tear wherever it started.
- A turned frame is read a screen row at a time while it is turned, a run of
  neighbouring pixels from each, instead of one pixel per screen row: a whole
  frame in PSRAM, turned on the way out, went from 47.8 fps to 60 on the
  T-Deck Plus.
- `asyncPresent` and `scanlineSync` are on by default: a board gets frames
  sent from the other core, and tear-free frames without a TE line, where it
  can. Where it cannot (one core, no PSRAM for the copies, a panel that
  cannot be read back) frames are sent as they are drawn, and the boot log
  says so as information, not an error. Scenes that set them keep what they
  set.
