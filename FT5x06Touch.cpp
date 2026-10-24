// Portions derived from LovyanGFX (https://github.com/lovyan03/LovyanGFX),
// Copyright (c) lovyan03 and contributors, FreeBSD licence: Touch_FT5x06
// (init, _check_init, getTouchRaw). See NOTICE.

#include "FT5x06Touch.h"

#include <cstring>

namespace DekiDisplay
{

namespace
{
constexpr uint8_t kFtRegCount = 0x02;    // fingers down
constexpr uint8_t kFtRegPoints = 0x03;   // six bytes a finger
constexpr uint8_t kFtRegCipher = 0xA3;   // chip id, then firmware and vendor ids
constexpr uint8_t kFtRegIntMode = 0xA4;  // 0: INT low while touched (polling)
constexpr uint8_t kFtRegPower = 0xA5;
constexpr uint8_t kFtPowerMonitor = 0x01;
constexpr uint8_t kFtPowerSleep = 0x03;
constexpr int kFtPointBytes = 6;
}  // namespace

FT5x06Touch::FT5x06Touch(DekiI2c::IDekiI2C* bus, DekiGpio::IDekiGPIO* gpio, uint8_t address, int32_t intPin,
                         int32_t rstPin, DelayFn delay)
    : m_Bus(bus),
      m_Gpio(gpio),
      m_Address(address ? address : kAddress),
      m_IntPin(gpio ? intPin : -1),
      m_RstPin(gpio ? rstPin : -1),
      m_Delay(delay)
{
}

bool FT5x06Touch::WriteReg(uint8_t reg, uint8_t value)
{
    return m_Bus->Write(m_Address, reg, &value, 1);
}

bool FT5x06Touch::Init()
{
    if (!m_Bus)
    {
        return false;
    }
    if (m_RstPin >= 0)
    {
        m_Gpio->SetOutput(m_RstPin, false);
        if (m_Delay)
        {
            m_Delay(1);
        }
        m_Gpio->Write(m_RstPin, true);
    }
    if (m_IntPin >= 0)
    {
        m_Gpio->SetInput(m_IntPin, DekiGpio::Pull::Up);
    }
    // The ids, the last of which (the vendor's) is never 0 on a chip that
    // answers.
    uint8_t ids[6] = {};
    return WriteReg(0x00, 0x00) && m_Bus->Read(m_Address, kFtRegCipher, ids, sizeof(ids)) &&
           WriteReg(kFtRegIntMode, 0x00) && ids[5] != 0;
}

int FT5x06Touch::Read(TouchPoint* points, int max)
{
    if (!m_Bus || max <= 0)
    {
        return 0;
    }
    // Polling mode holds INT low while touched.
    if (m_IntPin >= 0 && m_Gpio->Read(m_IntPin))
    {
        return 0;
    }
    uint8_t count = 0;
    if (!m_Bus->Read(m_Address, kFtRegCount, &count, 1))
    {
        return 0;
    }
    int n = count & 0x0F;
    if (n == 0)
    {
        return 0;
    }
    n = n < kMaxPoints ? n : kMaxPoints;
    n = n < max ? n : max;
    // The points can change while they are read: read them until two reads
    // agree, as LovyanGFX does.
    uint8_t data[2][kMaxPoints * kFtPointBytes] = {};
    const size_t bytes = static_cast<size_t>(n) * kFtPointBytes;
    if (!m_Bus->Read(m_Address, kFtRegPoints, data[0], bytes))
    {
        return 0;
    }
    for (int retry = 0; retry < 4; ++retry)
    {
        if (!m_Bus->Read(m_Address, kFtRegPoints, data[1], bytes) || memcmp(data[0], data[1], bytes) == 0)
        {
            break;
        }
        memcpy(data[0], data[1], bytes);
    }
    // Each finger: event and x high nibble, x low, id and y high nibble, y low.
    for (int i = 0; i < n; ++i)
    {
        const uint8_t* d = &data[0][i * kFtPointBytes];
        points[i].x = static_cast<uint16_t>(((d[0] & 0x0F) << 8) | d[1]);
        points[i].y = static_cast<uint16_t>(((d[2] & 0x0F) << 8) | d[3]);
        points[i].id = static_cast<uint8_t>(d[2] >> 4);
        points[i].size = 1;
    }
    return n;
}

void FT5x06Touch::Sleep()
{
    if (m_Bus)
    {
        WriteReg(kFtRegPower, kFtPowerSleep);
    }
}

void FT5x06Touch::Wake()
{
    if (!m_Bus)
    {
        return;
    }
    // A pulse on INT wakes it.
    if (m_IntPin >= 0)
    {
        m_Gpio->SetInput(m_IntPin, DekiGpio::Pull::Down);
        if (m_Delay)
        {
            m_Delay(1);
        }
        m_Gpio->SetInput(m_IntPin, DekiGpio::Pull::Up);
    }
    WriteReg(kFtRegPower, kFtPowerMonitor);
}

}  // namespace DekiDisplay
