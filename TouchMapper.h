#pragma once

#include <cstdint>

namespace DekiDisplay
{

/// Turns a touch controller's raw coordinates into screen pixels: the raw
/// range scaled to the panel, then turned with the display's rotation plus
/// the controller's own turn against the panel. The same arithmetic as
/// LovyanGFX's default calibration and Panel_Device::convertRawXY, so a
/// board's touch settings carry over unchanged.
struct TouchMapper
{
    /// The raw range, along the panel's own width (x) and height (y) before
    /// the display is rotated: a 240x320 panel's controller reports 0-239 and
    /// 0-319.
    int32_t xMin = 0;
    int32_t xMax = 239;
    int32_t yMin = 0;
    int32_t yMax = 319;
    /// The panel's visible size before rotation.
    int32_t panelWidth = 240;
    int32_t panelHeight = 320;
    /// The display's rotation (0-7: quarter turns, then mirrored).
    uint8_t displayRotation = 0;
    /// How the controller is turned against the panel (0-7), added to the
    /// display's rotation.
    uint8_t offsetRotation = 0;

    /// `rawX`, `rawY` as a pixel of the screen, as the display is rotated.
    void Map(int32_t rawX, int32_t rawY, int32_t& screenX, int32_t& screenY) const;
};

}  // namespace DekiDisplay
