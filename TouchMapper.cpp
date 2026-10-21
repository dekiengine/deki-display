// Portions derived from LovyanGFX (https://github.com/lovyan03/LovyanGFX),
// Copyright (c) lovyan03 and contributors, FreeBSD licence: the default touch
// calibration (Panel_Device::touchCalibrate) and convertRawXY. See NOTICE.

#include "TouchMapper.h"

#include <utility>

namespace DekiDisplay
{

void TouchMapper::Map(int32_t rawX, int32_t rawY, int32_t& screenX, int32_t& screenY) const
{
    // The raw corners onto the panel's corners, the panel taken as the
    // controller is turned against it.
    int32_t w = panelWidth;
    int32_t h = panelHeight;
    if (offsetRotation & 1)
    {
        std::swap(w, h);
    }
    const int32_t spanX = xMax != xMin ? xMax - xMin : 1;
    const int32_t spanY = yMax != yMin ? yMax - yMin : 1;
    int32_t tx = (rawX - xMin) * (w - 1) / spanX;
    int32_t ty = (rawY - yMin) * (h - 1) / spanY;

    // Then turned as the screen is: the display's rotation plus the
    // controller's own, the mirror bits combined.
    const uint8_t r = static_cast<uint8_t>(((displayRotation + offsetRotation) & 3) |
                                           ((displayRotation & 4) ^ (offsetRotation & 4)));
    // The screen's size as rotated.
    const int32_t width = (displayRotation & 1) ? panelHeight : panelWidth;
    const int32_t height = (displayRotation & 1) ? panelWidth : panelHeight;
    if (r & 1)
    {
        std::swap(tx, ty);
    }
    if (r & 2)
    {
        tx = (width - 1) - tx;
    }
    // Rotations 1, 2, 4 and 7 count rows from the other edge.
    if ((1 << r) & 0b10010110)
    {
        ty = (height - 1) - ty;
    }
    screenX = tx;
    screenY = ty;
}

}  // namespace DekiDisplay
