#include "ScanlineClock.h"

namespace DekiDisplay
{

namespace
{
constexpr uint8_t kGetScanline = 0x45;
// A drop to this low byte or less from at least kDropFrom above it is where
// a refresh may start.
constexpr uint8_t kStartLow = 4;
constexpr uint8_t kDropFrom = 12;
// After a refresh's last row the count holds through the blanking, a few
// hundred microseconds; after its low byte rolls over (row 255 to 256) it
// goes on at a row every ~50 us. Held this long, the drop was a start.
constexpr uint32_t kBlankingHoldUs = 150;
// How far either side of a start's predicted time WaitForStart looks for it.
constexpr uint32_t kCatchWindowUs = 500;
// A refresh lasts between these: 25 to 125 Hz.
constexpr uint32_t kShortestRefreshUs = 8000;
constexpr uint32_t kLongestRefreshUs = 40000;
}  // namespace

ScanlineClock::ScanlineClock(IPanelIO& io, MicrosFn micros, YieldFn yield)
    : m_IO(io),
      m_Micros(micros),
      m_Yield(yield)
{
}

uint8_t ScanlineClock::DecodeLow(const uint8_t reply[3])
{
    return static_cast<uint8_t>(((reply[1] & 0x7F) << 1) | (reply[2] >> 7));
}

bool ScanlineClock::ReadLow(uint8_t& low)
{
    uint8_t reply[3] = {};
    if (!m_IO.Read(kGetScanline, reply, sizeof(reply)))
    {
        return false;
    }
    low = DecodeLow(reply);
    return true;
}

bool ScanlineClock::CatchStart(uint64_t deadline, uint64_t& at)
{
    uint8_t prev = 0;
    if (!ReadLow(prev))
    {
        return false;
    }
    uint64_t prevUs = m_Micros();
    while (prevUs < deadline)
    {
        uint8_t low = 0;
        if (!ReadLow(low))
        {
            return false;
        }
        const uint64_t now = m_Micros();
        if (low <= kStartLow && prev >= low + kDropFrom)
        {
            // A start if the count now holds through the blanking.
            const uint64_t dropUs = prevUs + (now - prevUs) / 2;
            uint8_t next = low;
            uint64_t nextUs = now;
            while (next == low && nextUs - now < kBlankingHoldUs)
            {
                if (!ReadLow(next))
                {
                    return false;
                }
                nextUs = m_Micros();
            }
            if (next == low)
            {
                at = dropUs;
                return true;
            }
            low = next;
            prevUs = nextUs;
            prev = low;
            continue;
        }
        prev = low;
        prevUs = now;
    }
    return false;
}

bool ScanlineClock::Calibrate()
{
    m_PeriodUs = 0;
    uint64_t first = 0;
    uint64_t second = 0;
    if (!CatchStart(m_Micros() + 2 * kLongestRefreshUs, first) ||
        !CatchStart(first + kLongestRefreshUs + kCatchWindowUs, second))
    {
        return false;
    }
    const uint64_t period = second - first;
    if (period < kShortestRefreshUs || period > kLongestRefreshUs)
    {
        return false;
    }
    m_FirstStartUs = first;
    m_LastStartUs = second;
    m_PeriodUs = static_cast<uint32_t>(period);
    return true;
}

uint32_t ScanlineClock::AgeUs(uint64_t now) const
{
    if (m_PeriodUs == 0 || now < m_LastStartUs)
    {
        return 0;
    }
    return static_cast<uint32_t>((now - m_LastStartUs) % m_PeriodUs);
}

void ScanlineClock::WaitUntil(uint64_t until) const
{
    for (uint64_t now = m_Micros(); now < until; now = m_Micros())
    {
        if (until - now > 2000 && m_Yield)
        {
            m_Yield();
        }
    }
}

void ScanlineClock::WaitForAge(uint32_t ageUs)
{
    const uint64_t now = m_Micros();
    const uint32_t age = AgeUs(now);
    if (age < ageUs)
    {
        WaitUntil(now + (ageUs - age));
    }
}

void ScanlineClock::WaitForStart()
{
    if (m_PeriodUs == 0)
    {
        return;
    }
    const uint64_t now = m_Micros();
    const uint64_t next = now + (m_PeriodUs - AgeUs(now));
    WaitUntil(next > kCatchWindowUs ? next - kCatchWindowUs : 0);
    uint64_t at = 0;
    if (!CatchStart(next + kCatchWindowUs, at))
    {
        // Not seen where expected: go on the clock, and catch the next one.
        WaitUntil(next);
        return;
    }
    // The period over every refresh since the first start: the longer the
    // baseline, the less a read's timing matters.
    const uint64_t since = at - m_FirstStartUs;
    const uint64_t refreshes = (since + m_PeriodUs / 2) / m_PeriodUs;
    if (refreshes > 0)
    {
        const uint64_t period = since / refreshes;
        if (period >= kShortestRefreshUs && period <= kLongestRefreshUs)
        {
            m_PeriodUs = static_cast<uint32_t>(period);
        }
    }
    m_LastStartUs = at;
}

}  // namespace DekiDisplay
