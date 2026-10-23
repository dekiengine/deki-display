// Portions derived from LovyanGFX (https://github.com/lovyan03/LovyanGFX),
// Copyright (c) lovyan03 and contributors, FreeBSD licence: Panel_LCD's
// rotation arithmetic and window encoding, Panel_Device's reset timing. See
// NOTICE.

#include "PanelController.h"

#include "DekiGPIO.h"  // from deki-gpio
#include <deki/LogSystem.h>

#include <utility>

namespace DekiDisplay
{

namespace
{
constexpr uint8_t kCmdNop = 0x00;
constexpr uint8_t kCmdSlpIn = 0x10;
constexpr uint8_t kCmdSlpOut = 0x11;
constexpr uint8_t kCmdInvOff = 0x20;
constexpr uint8_t kCmdInvOn = 0x21;
constexpr uint8_t kCmdDispOff = 0x28;
constexpr uint8_t kCmdDispOn = 0x29;
constexpr uint8_t kCmdCaset = 0x2A;
constexpr uint8_t kCmdRaset = 0x2B;
constexpr uint8_t kCmdRamwr = 0x2C;
constexpr uint8_t kCmdMadctl = 0x36;
constexpr uint8_t kCmdColmod = 0x3A;
constexpr uint8_t kColmodRGB565 = 0x55;
}  // namespace

PanelController::PanelController(IPanelIO& io, DelayFn delay)
    : m_IO(io),
      m_Delay(delay),
      m_Info(&GetControllerInfo(Controller::ST7789))
{
}

bool PanelController::Init(const PanelConfig& config, const InitSequence* init)
{
    m_Config = config;
    m_Info = &GetControllerInfo(config.controller);
    if (m_Config.memoryWidth <= 0)
    {
        m_Config.memoryWidth = m_Info->memoryWidth;
    }
    if (m_Config.memoryHeight <= 0)
    {
        m_Config.memoryHeight = m_Info->memoryHeight;
    }

    // Reset as LovyanGFX does: low for 8 ms, high, then 64 ms for the
    // controller to start. The waits stay without a reset line, giving a
    // panel that has just been powered the same time.
    DekiGpio::IDekiGPIO* gpio = DekiGpio::DekiGPIO::GetCurrent();
    if (gpio && m_Config.resetPin >= 0)
    {
        gpio->SetOutput(m_Config.resetPin, false);
    }
    if (m_Delay)
    {
        m_Delay(8);
    }
    if (gpio && m_Config.resetPin >= 0)
    {
        gpio->Write(m_Config.resetPin, true);
    }
    if (m_Delay)
    {
        m_Delay(64);
    }

    const InitSequence defaults = InitSequenceFromTable(m_Info->defaultInit);
    const InitSequence& steps = (init && !init->empty()) ? *init : defaults;
    if (!m_IO.Begin())
    {
        DEKI_LOG_ERROR("PanelController: cannot take the bus to start the %s", m_Info->name);
        return false;
    }
    bool ok = true;
    for (const InitStep& step : steps)
    {
        ok = m_IO.Command(step.cmd, step.params.data(), step.params.size()) && ok;
        if (step.delayMs > 0 && m_Delay)
        {
            m_Delay(step.delayMs);
        }
    }
    m_IO.End();

    ok = SetInvert(m_Config.invert) && ok;
    ok = m_IO.Command(kCmdColmod, &kColmodRGB565, 1) && ok;
    ok = SetRotation(m_Config.rotation) && ok;
    if (!ok)
    {
        DEKI_LOG_ERROR("PanelController: the %s did not take its start-up", m_Info->name);
    }
    return ok;
}

bool PanelController::WriteInOwnOrientation()
{
    const uint8_t screen = m_ScreenRotation;
    const bool ok = SetRotation(0);
    m_ScreenRotation = screen;
    return ok;
}

bool PanelController::SetRotation(uint8_t rotation)
{
    m_ScreenRotation = rotation & 7;
    // Panel_LCD::setRotation with no offset rotation.
    m_Rotation = rotation & 7;
    const uint8_t r = m_Rotation;
    int32_t ox = m_Config.offsetX;
    int32_t oy = m_Config.offsetY;
    int32_t pw = m_Config.panelWidth;
    int32_t ph = m_Config.panelHeight;
    int32_t mw = m_Config.memoryWidth;
    int32_t mh = m_Config.memoryHeight;
    if (r & 1)
    {
        std::swap(ox, oy);
        std::swap(pw, ph);
        std::swap(mw, mh);
    }
    m_Width = pw;
    m_Height = ph;
    if (m_Info->offsetsFollowRotation)
    {
        m_ColStart = (r & 2) ? mw - (pw + ox) : ox;
        // Rotations 1, 2, 4 and 7 count rows from the other end of memory.
        m_RowStart = ((1 << r) & 0b10010110) ? mh - (ph + oy) : oy;
    }
    else
    {
        // Panel_AMOLED: the offset as it is, only swapped with the axes.
        m_ColStart = ox;
        m_RowStart = oy;
    }
    ForgetWindow();
    return SendMadctl();
}

bool PanelController::SendMadctl()
{
    const uint8_t madctl = static_cast<uint8_t>(m_Info->madctl[m_Rotation] | (m_Config.rgbOrder ? 0 : m_Info->bgrBit));
    return m_IO.Command(kCmdMadctl, &madctl, 1);
}

bool PanelController::SetInvert(bool invert)
{
    m_Config.invert = invert;
    return m_IO.Command(invert ? kCmdInvOn : kCmdInvOff, nullptr, 0);
}

bool PanelController::SetSleep(bool sleep)
{
    const bool ok = m_IO.Command(sleep ? kCmdSlpIn : kCmdSlpOut, nullptr, 0);
    // The controller needs 120 ms after SLPOUT before the next SLPIN, and
    // 5 ms after SLPIN before it is asleep.
    if (m_Delay)
    {
        m_Delay(sleep ? 5 : 120);
    }
    return ok;
}

bool PanelController::SetDisplayOn(bool on)
{
    return m_IO.Command(on ? kCmdDispOn : kCmdDispOff, nullptr, 0);
}

bool PanelController::SetBrightness(uint8_t level)
{
    return HasBrightness() && m_IO.Command(m_Info->brightnessCmd, &level, 1);
}

bool PanelController::BeginWrite()
{
    return m_IO.Begin();
}

void PanelController::EndWrite()
{
    // With no chip select the panel reads everything as one write until a
    // command: the NOP lets another device use the bus between writes.
    if (!m_Config.hasChipSelect && m_Info->nopOnEnd)
    {
        m_IO.Command(kCmdNop, nullptr, 0);
    }
    m_IO.End();
}

static void PutWindow(uint8_t* out, int32_t start, int32_t end)
{
    out[0] = static_cast<uint8_t>(start >> 8);
    out[1] = static_cast<uint8_t>(start);
    out[2] = static_cast<uint8_t>(end >> 8);
    out[3] = static_cast<uint8_t>(end);
}

bool PanelController::SetWindow(int32_t x0, int32_t y0, int32_t x1, int32_t y1)
{
    if (x1 <= x0 || y1 <= y0)
    {
        return false;
    }
    const int32_t xs = x0 + m_ColStart;
    const int32_t xe = x1 - 1 + m_ColStart;
    const int32_t ys = y0 + m_RowStart;
    const int32_t ye = y1 - 1 + m_RowStart;
    bool ok = true;
    uint8_t params[4];
    if (xs != m_Xs || xe != m_Xe)
    {
        PutWindow(params, xs, xe);
        ok = m_IO.Command(kCmdCaset, params, 4);
        m_Xs = ok ? xs : -1;
        m_Xe = ok ? xe : -1;
    }
    if (ok && (ys != m_Ys || ye != m_Ye))
    {
        PutWindow(params, ys, ye);
        ok = m_IO.Command(kCmdRaset, params, 4);
        m_Ys = ok ? ys : -1;
        m_Ye = ok ? ye : -1;
    }
    return ok && m_IO.StartPixels(kCmdRamwr);
}

bool PanelController::SendCommand(uint8_t cmd, const uint8_t* params, size_t n)
{
    ForgetWindow();
    return m_IO.Command(cmd, params, n);
}

void PanelController::ForgetWindow()
{
    m_Xs = m_Xe = m_Ys = m_Ye = -1;
}

}  // namespace DekiDisplay
