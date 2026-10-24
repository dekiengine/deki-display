#pragma once

#include "IPanelIO.h"

#include <cstdint>

namespace DekiDisplay
{

/// Where a panel is in its refresh, from its scanline (MIPI DCS 0x45,
/// Get_Scanline): for a panel whose TE line is not wired.
///
/// A refresh's start is caught where the line count drops back from the
/// panel's last row to the blanking before the first, so only the low byte
/// of the count is used (the ST7789 gives its high bit late). Two starts make
/// the clock; each start caught after that keeps it right as the panel's
/// oscillator and the chip's drift apart. Between starts the refresh is where
/// the clock says, with no read.
class ScanlineClock
{
public:
    using MicrosFn = uint64_t (*)();
    using YieldFn = void (*)();

    /// `io` reads the panel; `micros` is a monotonic clock; `yield` lets
    /// other tasks run while it waits (null: it spins).
    ScanlineClock(IPanelIO& io, MicrosFn micros, YieldFn yield);

    /// Finds two refresh starts, in up to three refreshes. False when the
    /// scanline does not move (a panel that cannot be read).
    bool Calibrate();

    bool IsCalibrated() const { return m_PeriodUs != 0; }
    /// How long a refresh takes.
    uint32_t GetPeriodUs() const { return m_PeriodUs; }
    /// Microseconds since the refresh going on at `now` started.
    uint32_t AgeUs(uint64_t now) const;
    /// Waits for the next refresh to start, and catches it from the
    /// scanline to keep the clock right. Returns when it starts.
    void WaitForStart();
    /// Waits until the refresh going on is `ageUs` old, or returns at once
    /// when it is older.
    void WaitForAge(uint32_t ageUs);

    /// The low byte of the line count in a Get_Scanline reply, as read with
    /// the controller's dummy bit first: the first byte's low seven bits and
    /// the second's top bit are the count's high byte, then the low byte.
    static uint8_t DecodeLow(const uint8_t reply[3]);

private:
    // Reads the low byte of the line count.
    bool ReadLow(uint8_t& low);
    // Polls until a refresh starts or `deadline` passes; `at` is when it
    // started, between the two reads that saw it.
    bool CatchStart(uint64_t deadline, uint64_t& at);
    // Spins until `until`, letting other tasks run while more than 2 ms are left.
    void WaitUntil(uint64_t until) const;

    IPanelIO& m_IO;
    MicrosFn m_Micros;
    YieldFn m_Yield;
    uint64_t m_FirstStartUs = 0;  // the first start caught: the long baseline
    uint64_t m_LastStartUs = 0;   // the latest
    uint32_t m_PeriodUs = 0;
};

}  // namespace DekiDisplay
