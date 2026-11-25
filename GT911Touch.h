#pragma once

#include <cstdint>
#include "ITouchController.h"
#include "IDekiGPIO.h"  // from deki-gpio
#include "IDekiI2C.h"   // from deki-i2c

namespace DekiDisplay
{

/// The Goodix GT911 capacitive touch controller, on a deki-i2c bus.
///
/// Its registers are 16-bit: the status at 0x814E says a report is ready
/// (bit 7) and how many fingers it holds; their points follow from 0x814F,
/// 8 bytes each; writing 0 to the status asks for the next report. With an
/// INT pin wired and the controller set to hold INT at a level while a
/// report waits (0x804D, as most boards ship it), it is read only then; set
/// to pulse INT instead (rising or falling edge), a pulse comes and goes
/// between two frames, so the status is read every frame, as without INT.
///
/// Portions derived from LovyanGFX (https://github.com/lovyan03/LovyanGFX),
/// Copyright (c) lovyan03 and contributors, FreeBSD licence: Touch_GT911's
/// start-up, wake-up and the way it drops a report left from long ago.
class GT911Touch : public ITouchController
{
public:
    using DelayFn = void (*)(uint32_t ms);
    using NowFn = uint32_t (*)();

    static constexpr uint8_t kAddress = 0x5D;
    static constexpr uint8_t kAlternateAddress = 0x14;
    static constexpr int kMaxPoints = 5;

    /// `gpio` may be null when neither `intPin` nor `rstPin` is wired.
    GT911Touch(DekiI2c::IDekiI2C* bus, DekiGpio::IDekiGPIO* gpio, uint8_t address, int32_t intPin, int32_t rstPin,
               DelayFn delay, NowFn now);

    bool Init() override;
    int Read(TouchPoint* points, int max) override;
    void Sleep() override;
    void Wake() override;

    /// The address it answered on: the GT911 takes 0x5D or 0x14, as its INT
    /// pin was at reset.
    uint8_t GetAddress() const { return m_Address; }

private:
    bool ClearStatus();
    // Reads a report waiting in the controller into m_Report. False when
    // there is none.
    bool UpdateReport();

    DekiI2c::IDekiI2C* m_Bus;
    DekiGpio::IDekiGPIO* m_Gpio;
    uint8_t m_Address;
    int32_t m_IntPin;
    int32_t m_RstPin;
    DelayFn m_Delay;
    NowFn m_Now;
    bool m_Started = false;
    uint32_t m_LastRead = 0;
    // How INT says a report waits (0x804D, bits 0 and 1): 0 a rising edge,
    // 1 a falling edge, 2 held low, 3 held high; -1 when it could not be read
    // (taken as held low, as before).
    int m_IntMode = -1;
    bool ReportWaiting() const;
    // The last report: the status byte, then 8 bytes per point.
    uint8_t m_Report[1 + kMaxPoints * 8] = {};
};

}  // namespace DekiDisplay
