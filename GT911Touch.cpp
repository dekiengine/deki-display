// Portions derived from LovyanGFX (https://github.com/lovyan03/LovyanGFX),
// Copyright (c) lovyan03 and contributors, FreeBSD licence: Touch_GT911 (init,
// wakeup, sleep, _update_data, getTouchRaw). See NOTICE.

#include "GT911Touch.h"

#include <algorithm>

namespace DekiDisplay
{

namespace
{
constexpr uint8_t kStatus[] = { 0x81, 0x4E };
constexpr uint8_t kPoints[] = { 0x81, 0x4F };
constexpr uint8_t kClearStatus[] = { 0x81, 0x4E, 0x00 };
constexpr uint8_t kSleep[] = { 0x80, 0x40, 0x05 };
}  // namespace

GT911Touch::GT911Touch(DekiI2c::IDekiI2C* bus, DekiGpio::IDekiGPIO* gpio, uint8_t address, int32_t intPin,
                       int32_t rstPin, DelayFn delay, NowFn now)
    : m_Bus(bus),
      m_Gpio(gpio),
      m_Address(address ? address : kAddress),
      m_IntPin(gpio ? intPin : -1),
      m_RstPin(gpio ? rstPin : -1),
      m_Delay(delay),
      m_Now(now)
{
}

bool GT911Touch::ClearStatus()
{
    return m_Bus->WriteRaw(m_Address, kClearStatus, sizeof(kClearStatus));
}

bool GT911Touch::Init()
{
    if (m_Started)
    {
        return true;
    }
    if (!m_Bus)
    {
        return false;
    }
    m_Report[0] = 0;
    if (m_RstPin >= 0)
    {
        m_Gpio->SetOutput(m_RstPin, false);
        if (m_Delay)
        {
            m_Delay(5);
        }
        m_Gpio->Write(m_RstPin, true);
    }
    // It answers on 0x5D or 0x14, as INT was at reset: try one, then the
    // other.
    for (int retry = 6; retry > 0; --retry)
    {
        if (ClearStatus())
        {
            m_Started = true;
            Wake();
            return true;
        }
        if (m_Delay)
        {
            m_Delay(1);
        }
        m_Address = m_Address == kAddress ? kAlternateAddress : kAddress;
    }
    return false;
}

void GT911Touch::Wake()
{
    if (!m_Started || m_IntPin < 0)
    {
        return;
    }
    // A low pulse on INT wakes it; then INT is its report line again.
    m_Gpio->SetOutput(m_IntPin, false);
    if (m_Delay)
    {
        m_Delay(58);
    }
    m_Gpio->Write(m_IntPin, true);
    if (m_Delay)
    {
        m_Delay(2);
    }
    m_Gpio->SetInput(m_IntPin, DekiGpio::Pull::Up);
}

void GT911Touch::Sleep()
{
    if (!m_Started)
    {
        return;
    }
    if (m_IntPin >= 0)
    {
        m_Gpio->SetOutput(m_IntPin, false);
    }
    m_Bus->WriteRaw(m_Address, kSleep, sizeof(kSleep));
}

bool GT911Touch::UpdateReport()
{
    uint8_t status = 0;
    if (!m_Bus->WriteRead(m_Address, kStatus, sizeof(kStatus), &status, 1) || !(status & 0x80))
    {
        return false;
    }
    const int points = std::min<int>(kMaxPoints, status & 0x0F);
    if (points > 0 && !m_Bus->WriteRead(m_Address, kPoints, sizeof(kPoints), &m_Report[1], points * 8))
    {
        return false;
    }
    m_Report[0] = status;
    // Read: the controller may fill the next report.
    ClearStatus();
    return true;
}

int GT911Touch::Read(TouchPoint* points, int max)
{
    if (!m_Started || max <= 0)
    {
        return 0;
    }
    // INT high: no new report, and the last one still holds.
    if (m_IntPin < 0 || !m_Gpio->Read(m_IntPin))
    {
        const uint32_t now = m_Now ? m_Now() : 0;
        const uint32_t since = now - m_LastRead;
        m_LastRead = now;
        // The controller keeps a report until its status is cleared, so one
        // waiting from long ago is stale: drop it and wait a little for a
        // fresh one.
        if (since >= 128)
        {
            ClearStatus();
            for (int retry = 24; retry > 0; --retry)
            {
                if (m_Delay)
                {
                    m_Delay(1);
                }
                if (UpdateReport())
                {
                    break;
                }
            }
        }
        else
        {
            UpdateReport();
        }
    }
    const int count = std::min({ max, kMaxPoints, static_cast<int>(m_Report[0] & 0x0F) });
    for (int i = 0; i < count; ++i)
    {
        // Each point: track id, x, y and size, 16-bit values low byte first.
        const uint8_t* p = &m_Report[1 + i * 8];
        points[i].id = p[0];
        points[i].x = static_cast<uint16_t>(p[1] | (p[2] << 8));
        points[i].y = static_cast<uint16_t>(p[3] | (p[4] << 8));
        points[i].size = static_cast<uint16_t>(p[5] | (p[6] << 8));
    }
    return count;
}

}  // namespace DekiDisplay
