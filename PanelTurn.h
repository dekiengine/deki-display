#pragma once

#include <cstdint>

namespace DekiDisplay
{

/// A map between two pixel grids that only turns and mirrors:
/// x' = ox + xx * x + xy * y, y' = oy + yx * x + yy * y, the four factors
/// each -1, 0 or 1 (a quarter turn, a mirror, or both).
struct PixelMap
{
    int32_t ox = 0;
    int32_t oy = 0;
    int8_t xx = 1;
    int8_t xy = 0;
    int8_t yx = 0;
    int8_t yy = 1;

    void Apply(int32_t x, int32_t y, int32_t& outX, int32_t& outY) const
    {
        outX = ox + xx * x + xy * y;
        outY = oy + yx * x + yy * y;
    }

    /// `next` after this one.
    PixelMap Then(const PixelMap& next) const
    {
        PixelMap m;
        m.xx = static_cast<int8_t>(next.xx * xx + next.xy * yx);
        m.xy = static_cast<int8_t>(next.xx * xy + next.xy * yy);
        m.yx = static_cast<int8_t>(next.yx * xx + next.yy * yx);
        m.yy = static_cast<int8_t>(next.yx * xy + next.yy * yy);
        m.ox = next.ox + next.xx * ox + next.xy * oy;
        m.oy = next.oy + next.yx * ox + next.yy * oy;
        return m;
    }

    /// The map back. A turn or mirror's inverse is its transpose.
    PixelMap Inverse() const
    {
        PixelMap m;
        m.xx = xx;
        m.xy = yx;
        m.yx = xy;
        m.yy = yy;
        m.ox = -(m.xx * ox + m.xy * oy);
        m.oy = -(m.yx * ox + m.yy * oy);
        return m;
    }
};

/// Where a MADCTL value puts the screen's pixels in the panel's own grid,
/// for a visible panel `panelWidth` x `panelHeight` before rotation (offsets
/// in the controller's memory aside: the controller adds those). MV
/// exchanges the axes and MX and MY then mirror the panel's columns and rows,
/// as the MIPI DCS controllers' address counters do.
inline PixelMap MadctlMap(uint8_t madctl, int32_t panelWidth, int32_t panelHeight)
{
    constexpr uint8_t kMY = 0x80, kMX = 0x40, kMV = 0x20;
    PixelMap m;
    if (madctl & kMV)
    {
        m.xx = 0;
        m.xy = 1;
        m.yx = 1;
        m.yy = 0;
    }
    if (madctl & kMX)
    {
        m.ox = panelWidth - 1 - m.ox;
        m.xx = static_cast<int8_t>(-m.xx);
        m.xy = static_cast<int8_t>(-m.xy);
    }
    if (madctl & kMY)
    {
        m.oy = panelHeight - 1 - m.oy;
        m.yx = static_cast<int8_t>(-m.yx);
        m.yy = static_cast<int8_t>(-m.yy);
    }
    return m;
}

}  // namespace DekiDisplay
