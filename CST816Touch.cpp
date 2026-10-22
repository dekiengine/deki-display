// Portions derived from LovyanGFX (https://github.com/lovyan03/LovyanGFX),
// Copyright (c) lovyan03 and contributors, FreeBSD licence: Touch_CST816S
// (init, _check_init, getTouchRaw). See NOTICE.

#include "CST816Touch.h"

namespace DekiDisplay
{

namespace
{
constexpr uint8_t kRegCount = 0x02;     // fingers down, then the point
constexpr uint8_t kRegChipId = 0xA7;
constexpr uint8_t kRegSleep = 0xA5;
constexpr uint8_t kRegIrqCtl = 0xFA;    // when INT pulses
constexpr uint8_t kRegIrqPulse = 0xED;  // how long, in 0.1 ms
constexpr uint8_t kIrqOnChange = 0x20;  // a pulse on each change; while touched (0x40) blurs the touch
constexpr uint8_t kSleepIn = 0x03;

uint16_t Median3(uint16_t a, uint16_t b, uint16_t c)
{
    if (a > b)
    {
        const uint16_t t = a;
        a = b;
        b = t;
    }
    // a <= b: the median is b unless c is below it.
    return c < a ? a : (c < b ? c : b);
}
}  // namespace

CST816Touch::CST816Touch(DekiI2c::IDekiI2C* bus, DekiGpio::IDekiGPIO* gpio, uint8_t address, int32_t intPin,
                         int32_t rstPin, DelayFn delay)
    : m_Bus(bus),
      m_Gpio(gpio),
      m_Address(address ? address : kAddress),
      m_IntPin(gpio ? intPin : -1),
      m_RstPin(gpio ? rstPin : -1),
      m_Delay(delay)
{
}

bool CST816Touch::Init()
{
    m_SetUp = false;
    if (m_RstPin >= 0)
    {
        m_Gpio->SetOutput(m_RstPin, false);
        if (m_Delay)
        {
            m_Delay(10);
        }
        m_Gpio->Write(m_RstPin, true);
        if (m_Delay)
        {
            m_Delay(10);
        }
    }
    if (m_IntPin >= 0)
    {
        m_Gpio->SetInput(m_IntPin, DekiGpio::Pull::Up);
    }
    return m_Bus != nullptr;
}

bool CST816Touch::SetUp()
{
    const uint8_t zero = 0;
    uint8_t id[3] = {};
    if (!m_Bus->Write(m_Address, 0x00, &zero, 1) || !m_Bus->Read(m_Address, kRegChipId, id, sizeof(id)))
    {
        return false;
    }
    const uint8_t onChange = kIrqOnChange;
    const uint8_t pulse = 20;  // 2 ms
    m_Bus->Write(m_Address, kRegIrqCtl, &onChange, 1);
    m_Bus->Write(m_Address, kRegIrqPulse, &pulse, 1);
    m_SetUp = true;
    return true;
}

int CST816Touch::Read(TouchPoint* points, int max)
{
    if (!m_Bus || max <= 0)
    {
        return 0;
    }
    // Asleep, it does not answer: nothing is touching it.
    if (!m_SetUp && !SetUp())
    {
        return 0;
    }
    // The count, then the point: x high nibble, x low byte, y high, y low.
    uint8_t data[6] = {};
    int count = 0;
    for (int retry = 3; retry > 0; --retry)
    {
        if (!m_Bus->Read(m_Address, kRegCount, data, 5))
        {
            return 0;
        }
        count = data[0];
        // It reports one finger; more is a read caught mid-update.
        if (count <= 1)
        {
            break;
        }
        count = 0;
    }
    if (count == 0)
    {
        m_Samples = 0;  // the next touch starts afresh
        return 0;
    }
    uint16_t x = static_cast<uint16_t>(data[2] | ((data[1] & 0x0F) << 8));
    uint16_t y = static_cast<uint16_t>(data[4] | ((data[3] & 0x0F) << 8));
    Filter(x, y);
    points[0].id = 0;
    points[0].size = 1;
    points[0].x = x;
    points[0].y = y;
    return 1;
}

void CST816Touch::Filter(uint16_t& x, uint16_t& y)
{
    const bool repeat = m_Samples > 0 && x == m_X[m_Samples - 1] && y == m_Y[m_Samples - 1];
    if (!repeat)
    {
        if (m_Samples == 3)
        {
            m_X[0] = m_X[1];
            m_X[1] = m_X[2];
            m_Y[0] = m_Y[1];
            m_Y[1] = m_Y[2];
            m_Samples = 2;
        }
        m_X[m_Samples] = x;
        m_Y[m_Samples] = y;
        ++m_Samples;
    }
    if (m_Samples == 3)
    {
        x = Median3(m_X[0], m_X[1], m_X[2]);
        y = Median3(m_Y[0], m_Y[1], m_Y[2]);
    }
}

void CST816Touch::Sleep()
{
    const uint8_t sleepIn = kSleepIn;
    m_Bus->Write(m_Address, kRegSleep, &sleepIn, 1);
    m_SetUp = false;
    m_Samples = 0;
}

}  // namespace DekiDisplay
