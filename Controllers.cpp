// The controllers' defaults: start-up tables and MADCTL per rotation.
//
// Portions derived from LovyanGFX (https://github.com/lovyan03/LovyanGFX),
// Copyright (c) lovyan03 and contributors, FreeBSD licence: the start-up
// tables (src/lgfx/v1/panel/Panel_*.hpp, v1.2.29) and the MADCTL tables
// (Panel_LCD::getMadCtl). See NOTICE.

#include "PanelController.h"

namespace DekiDisplay
{

namespace
{

// The table format: command, count of parameters (+0x80 when a delay in ms
// follows them), the parameters, the delay; 0xFF 0xFF ends it.
constexpr uint8_t kDelay = 0x80;

// MIPI DCS MADCTL bits.
constexpr uint8_t kMY = 0x80;
constexpr uint8_t kMX = 0x40;
constexpr uint8_t kMV = 0x20;
constexpr uint8_t kML = 0x10;
constexpr uint8_t kBGR = 0x08;
constexpr uint8_t kMH = 0x04;

// LovyanGFX's Panel_ST7789 list0. A panel module with other voltages
// (the LilyGO T-Deck's) replaces it from its boot scene.
constexpr uint8_t kST7789Init[] = {
    0xB7, 1, 0x35,              // GCTRL: gate voltages
    0xBB, 1, 0x28,              // VCOMS (JLX240 datasheet)
    0xC0, 1, 0x0C,              // LCMCTRL
    0xC2, 2, 0x01, 0xFF,        // VDVVRHEN
    0xC3, 1, 0x10,              // VRHS
    0xC4, 1, 0x20,              // VDVSET
    0xD0, 2, 0xA4, 0xA1,        // PWCTRL1
    0xB0, 2, 0x00, 0xC0,        // RAMCTRL
    0xE0, 14, 0xD0, 0x00, 0x02, 0x07, 0x0A, 0x28, 0x32, 0x44, 0x42, 0x06, 0x0E, 0x12, 0x14, 0x17,  // PVGAMCTRL
    0xE1, 14, 0xD0, 0x00, 0x02, 0x07, 0x0A, 0x28, 0x31, 0x54, 0x47, 0x0E, 0x1C, 0x17, 0x1B, 0x1E,  // NVGAMCTRL
    0x11, 0 + kDelay, 130,      // SLPOUT
    0x38, 0,                    // IDMOFF
    0x29, 0,                    // DISPON
    0xFF, 0xFF,
};

// LovyanGFX's Panel_RM67162 list0: out of sleep, the OVSS voltages on pages 5
// and 1, landscape, RGB565, dark until the display is on, then brightness.
constexpr uint8_t kRM67162Init[] = {
    0xFE, 1, 0x00,              // page 0
    0x11, 0 + kDelay, 150,      // SLPOUT
    0xFE, 1, 0x05,              // page 5
    0x05, 1, 0x05,              // OVSS control: ELVSS -3.95 V
    0xFE, 1, 0x01,              // page 1
    0x73, 1, 0x25,              // OVSS voltage level -4.0 V
    0xFE, 1, 0x00,              // page 0
    0x36, 1, 0x60,              // MADCTL
    0x3A, 1, 0x55,              // COLMOD: RGB565
    0x51, 1, 0x00,              // brightness: dark
    0x29, 0 + kDelay, 150,      // DISPON
    0x51, 1, 0xAF,              // brightness: on
    0xFF, 0xFF,
};

// Panel_LCD::getMadCtl: rotations 0-3, then 4-7 mirrored.
constexpr ControllerInfo kControllers[] = {
    {
        "ST7789",
        240,
        320,
        kST7789Init,
        {
            0,
            kMV | kMX | kMH,
            kMX | kMH | kMY | kML,
            kMV | kMY | kML,
            kMY | kML,
            kMV,
            kMX | kMH,
            kMV | kMX | kMY | kMH | kML,
        },
        kBGR,
        true,
        1,
        true,
        0,
        0,
    },
    {
        // LovyanGFX's Panel_RM67162 and Panel_AMOLED: its own MADCTL per
        // rotation, no colour-order bit, offsets applied as they are, rows
        // in pairs (it ignores a write whose start or size is odd along its
        // short axis), and brightness by command, having no backlight.
        "RM67162",
        240,
        536,
        kRM67162Init,
        { 0x00, 0x60, 0xC0, 0xA0, 0xC2, 0x62, 0x02, 0xA2 },
        0,
        false,
        2,
        false,
        0x51,
        0xAF,
    },
};

}  // namespace

const ControllerInfo& GetControllerInfo(Controller c)
{
    const auto i = static_cast<size_t>(c);
    return i < sizeof(kControllers) / sizeof(kControllers[0]) ? kControllers[i] : kControllers[0];
}

}  // namespace DekiDisplay
