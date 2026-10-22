#pragma once

#include <cstdint>
#include "ITouchController.h"
#include "IDekiGPIO.h"  // from deki-gpio
#include "IDekiI2C.h"   // from deki-i2c

namespace DekiDisplay
{

/// The Hynitron CST816S/CST816T capacitive touch controller, on a deki-i2c
/// bus at 0x15: one finger, its count at register 0x02 and its point after it.
///
/// It sleeps when nothing touches it and then does not answer on I2C at all,
/// so a read it does not answer is "no touch", and it is set up (the INT
/// pulse on each change) the first time it does answer, not at start-up.
///
/// It also folds: in places its point flips between two spots about 12
/// pixels apart from one sample to the next (the T-Display S3 AMOLED's, at
/// x 386-398 of 536), which a slow drag shows as a twitch back and forth. So
/// while a finger is down each axis reports the median of the last three
/// samples, which a one-sample flip never wins. A sample counts once however
/// often it is read (a frame reads faster than the chip samples), and the
/// first two samples of a touch are reported as they are. It costs a drag
/// one sample (about 19 ms) of lag.
///
/// Portions derived from LovyanGFX (https://github.com/lovyan03/LovyanGFX),
/// Copyright (c) lovyan03 and contributors, FreeBSD licence: Touch_CST816S's
/// reset, register set-up and point decoding.
class CST816Touch : public ITouchController
{
public:
    using DelayFn = void (*)(uint32_t ms);

    static constexpr uint8_t kAddress = 0x15;

    /// `gpio` may be null when neither `intPin` nor `rstPin` is wired.
    CST816Touch(DekiI2c::IDekiI2C* bus, DekiGpio::IDekiGPIO* gpio, uint8_t address, int32_t intPin, int32_t rstPin,
                DelayFn delay);

    /// Resets it and readies INT. Always true: a sleeping CST816 cannot be
    /// told from a missing one until it is touched.
    bool Init() override;
    int Read(TouchPoint* points, int max) override;
    void Sleep() override;

    /// True once it has answered and taken its set-up.
    bool IsAwake() const { return m_SetUp; }

private:
    bool SetUp();
    // Adds a sample to the touch's last three, unless it is the one before
    // read again, and gives the median of each axis.
    void Filter(uint16_t& x, uint16_t& y);

    DekiI2c::IDekiI2C* m_Bus;
    DekiGpio::IDekiGPIO* m_Gpio;
    uint8_t m_Address;
    int32_t m_IntPin;
    int32_t m_RstPin;
    DelayFn m_Delay;
    bool m_SetUp = false;
    // This touch's last three samples, oldest first; m_Samples of them.
    uint16_t m_X[3] = {};
    uint16_t m_Y[3] = {};
    int m_Samples = 0;
};

}  // namespace DekiDisplay
