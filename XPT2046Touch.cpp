// Portions derived from LovyanGFX (https://github.com/lovyan03/LovyanGFX),
// Copyright (c) lovyan03 and contributors, FreeBSD licence: Touch_XPT2046
// (getTouchRaw). See NOTICE.

#include "XPT2046Touch.h"

#include <algorithm>
#include <cstring>

namespace DekiDisplay
{

namespace
{
// One measurement: Z1, Z2 (pressure), X, Y, each a command byte and two
// reply bytes, the reply one byte behind its command.
constexpr uint8_t kMeasureZ1 = 0xB1;
constexpr uint8_t kMeasureZ2 = 0xC1;
constexpr uint8_t kMeasureX = 0xD1;
constexpr uint8_t kMeasureY = 0x91;
// Seven measurements of 8 bytes, then one byte that powers it down.
constexpr int kMeasurements = 7;
constexpr int kExchangeBytes = kMeasurements * 8 + 1;
constexpr uint8_t kPowerDown = 0x80;
}  // namespace

XPT2046Touch::XPT2046Touch(DekiSpi::IDekiSPIDevice* device, DekiGpio::IDekiGPIO* gpio, int32_t intPin)
    : m_Device(device),
      m_Gpio(gpio),
      m_IntPin(gpio ? intPin : -1)
{
}

bool XPT2046Touch::Init()
{
    if (m_IntPin >= 0)
    {
        m_Gpio->SetInput(m_IntPin, DekiGpio::Pull::None);
    }
    return m_Device != nullptr;
}

int XPT2046Touch::Decode(const uint8_t* rx, TouchPoint& point)
{
    uint16_t xs[kMeasurements];
    uint16_t ys[kMeasurements];
    uint16_t zs[kMeasurements];
    int nx = 0;
    int ny = 0;
    int nz = 0;
    for (int i = 0; i < kMeasurements; ++i)
    {
        const uint8_t* d = &rx[i * 8];
        const int y = ((d[1] << 8) | d[2]) >> 3;
        const int x = ((d[5] << 8) | d[6]) >> 3;
        const int z = 0x3200 + y - x + ((((d[3] << 8) | d[4]) - ((d[7] << 8) | d[8])) >> 1);
        // Readings at the ends of the range are the plate not pressed.
        if (x > 128 && x <= 3968)
        {
            xs[nx++] = static_cast<uint16_t>(x);
        }
        if (y > 128 && y <= 3968)
        {
            ys[ny++] = static_cast<uint16_t>(y);
        }
        if (z > 0)
        {
            zs[nz++] = static_cast<uint16_t>(z);
        }
    }
    if (nx < 3 || ny < 3 || nz < 3)
    {
        return 0;
    }
    std::sort(xs, xs + nx);
    std::sort(ys, ys + ny);
    std::sort(zs, zs + nz);
    point.x = xs[nx / 2];
    point.y = ys[ny / 2];
    point.size = static_cast<uint16_t>(zs[nz / 2] >> 8);
    point.id = 0;
    return point.size ? 1 : 0;
}

int XPT2046Touch::Read(TouchPoint* points, int max)
{
    if (!m_Device || max <= 0)
    {
        return 0;
    }
    // PENIRQ is high while nothing presses the plate.
    if (m_IntPin >= 0 && m_Gpio->Read(m_IntPin))
    {
        return 0;
    }
    uint8_t tx[kExchangeBytes] = {};
    const uint8_t measure[8] = { kMeasureY, 0, kMeasureZ1, 0, kMeasureX, 0, kMeasureZ2, 0 };
    for (int i = 0; i < kMeasurements; ++i)
    {
        memcpy(&tx[i * 8], measure, sizeof(measure));
    }
    tx[kExchangeBytes - 1] = kPowerDown;
    uint8_t rx[kExchangeBytes] = {};
    DekiSpi::Transfer t;
    t.tx = tx;
    t.txBytes = sizeof(tx);
    t.rx = rx;
    t.rxBytes = sizeof(rx);
    if (!m_Device->Transmit(t))
    {
        return 0;
    }
    return Decode(rx, points[0]);
}

}  // namespace DekiDisplay
