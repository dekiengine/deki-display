#pragma once

#include <cstdint>

#include "ITouchController.h"
#include "IDekiGPIO.h"  // from deki-gpio
#include "IDekiSPI.h"   // from deki-spi

namespace DekiDisplay
{

/// The XPT2046 (and ADS7843) resistive touch controller, on a deki-spi
/// device: X, Y and pressure measured seven times each, the middle reading
/// of each kept. One finger. Its raw readings run from about 300 to 3900, so
/// the Touch Panel's bounds must be set for the panel (calibrated).
///
/// Portions derived from LovyanGFX (https://github.com/lovyan03/LovyanGFX),
/// Copyright (c) lovyan03 and contributors, FreeBSD licence: Touch_XPT2046's
/// measurement sequence and filtering.
class XPT2046Touch : public ITouchController
{
public:
    /// `device` is a full-duplex device from IDekiSPIBus::AddDevice, at
    /// 1 MHz or so; the bus keeps it. `gpio` may be null when `intPin` (the
    /// controller's PENIRQ, low while pressed) is not wired.
    XPT2046Touch(DekiSpi::IDekiSPIDevice* device, DekiGpio::IDekiGPIO* gpio, int32_t intPin);

    bool Init() override;
    int Read(TouchPoint* points, int max) override;
    /// Pressure says when the finger has gone.
    bool MayRepeatLastTouch() const override { return false; }

    /// The middle of each set of seven readings in a 57-byte exchange, as
    /// Read decodes it: 0 when too few readings were in range.
    static int Decode(const uint8_t* rx, TouchPoint& point);

private:
    DekiSpi::IDekiSPIDevice* m_Device;
    DekiGpio::IDekiGPIO* m_Gpio;
    int32_t m_IntPin;
};

}  // namespace DekiDisplay
