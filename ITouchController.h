#pragma once

#include <cstdint>

namespace DekiDisplay
{

/// One finger, as the touch controller reports it: raw coordinates, along the
/// panel's own width and height before any rotation.
struct TouchPoint
{
    uint16_t x = 0;
    uint16_t y = 0;
    uint8_t id = 0;
    uint16_t size = 0;
};

/// What a touch controller driver implements: starting the chip and reading
/// where it is touched. TouchMapper turns its raw coordinates into screen
/// pixels.
class ITouchController
{
public:
    virtual ~ITouchController() = default;

    /// Starts the controller. False when it does not answer.
    virtual bool Init() = 0;
    /// Fills up to `max` points and returns how many fingers are down; 0 when
    /// none.
    virtual int Read(TouchPoint* points, int max) = 0;
    virtual void Sleep() {}
    virtual void Wake() {}
    /// True when, with no INT line, it may keep reporting the last touch
    /// after the finger has gone, so a point that does not move for a few
    /// frames counts as lifted.
    virtual bool MayRepeatLastTouch() const { return true; }
};

}  // namespace DekiDisplay
