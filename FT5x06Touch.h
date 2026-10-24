#pragma once

#include <cstdint>

#include "ITouchController.h"
#include "IDekiGPIO.h"  // from deki-gpio
#include "IDekiI2C.h"   // from deki-i2c

namespace DekiDisplay
{

/// The FocalTech FT5x06 family (FT5206, FT5306, FT5406, FT6206, FT6336 and
/// others) capacitive touch controller, on a deki-i2c bus at 0x38: up to five
/// fingers, their count at register 0x02 and six bytes a finger after it.
///
/// Portions derived from LovyanGFX (https://github.com/lovyan03/LovyanGFX),
/// Copyright (c) lovyan03 and contributors, FreeBSD licence: Touch_FT5x06's
/// reset, register set-up and point decoding.
class FT5x06Touch : public ITouchController
{
public:
    using DelayFn = void (*)(uint32_t ms);

    static constexpr uint8_t kAddress = 0x38;
    static constexpr int kMaxPoints = 5;

    /// `gpio` may be null when neither `intPin` nor `rstPin` is wired.
    FT5x06Touch(DekiI2c::IDekiI2C* bus, DekiGpio::IDekiGPIO* gpio, uint8_t address, int32_t intPin, int32_t rstPin,
                DelayFn delay);

    /// Resets it, readies INT and sets it to report by polling. False when it
    /// does not answer.
    bool Init() override;
    int Read(TouchPoint* points, int max) override;
    void Sleep() override;
    void Wake() override;
    /// Its count says when the finger has gone.
    bool MayRepeatLastTouch() const override { return false; }

    uint8_t GetAddress() const { return m_Address; }

private:
    bool WriteReg(uint8_t reg, uint8_t value);

    DekiI2c::IDekiI2C* m_Bus;
    DekiGpio::IDekiGPIO* m_Gpio;
    uint8_t m_Address;
    int32_t m_IntPin;
    int32_t m_RstPin;
    DelayFn m_Delay;
};

}  // namespace DekiDisplay
