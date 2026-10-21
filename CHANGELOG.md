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
