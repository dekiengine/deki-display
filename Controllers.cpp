// The controllers' defaults: start-up tables, MADCTL per rotation and quirks.
//
// Portions derived from LovyanGFX (https://github.com/lovyan03/LovyanGFX),
// Copyright (c) lovyan03 and contributors, FreeBSD licence: the start-up
// tables (src/lgfx/v1/panel/Panel_*.hpp, v1.2.29), the MADCTL tables
// (Panel_LCD::getMadCtl, Panel_GC9xxx, Panel_AMOLED, Panel_SSD1351) and the
// SSD1351's commands. See NOTICE.

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
constexpr uint8_t kMH = 0x04;

// The tables keep one command a line, laid out by hand.
// clang-format off
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

// LovyanGFX's Panel_ILI9341 list0.
constexpr uint8_t kILI9341Init[] = {
    0xEF, 3, 0x03, 0x80, 0x02,
    0xCF, 3, 0x00, 0xC1, 0x30,
    0xED, 4, 0x64, 0x03, 0x12, 0x81,
    0xE8, 3, 0x85, 0x00, 0x78,
    0xCB, 5, 0x39, 0x2C, 0x00, 0x34, 0x02,
    0xF7, 1, 0x20,
    0xEA, 2, 0x00, 0x00,
    0xC0, 1, 0x23,              // PWCTR1
    0xC1, 1, 0x10,              // PWCTR2
    0xC5, 2, 0x3E, 0x28,        // VMCTR1
    0xC7, 1, 0x86,              // VMCTR2
    0xB1, 2, 0x00, 0x13,        // FRMCTR1
    0xF2, 1, 0x00,
    0x26, 1, 0x01,              // GAMMASET: curve 1
    0xE0, 15, 0x0F, 0x31, 0x2B, 0x0C, 0x0E, 0x08, 0x4E, 0xF1, 0x37, 0x07, 0x10, 0x03, 0x0E, 0x09, 0x00,  // GMCTRP1
    0xE1, 15, 0x00, 0x0E, 0x14, 0x03, 0x11, 0x07, 0x31, 0xC1, 0x48, 0x08, 0x0F, 0x0C, 0x31, 0x36, 0x0F,  // GMCTRN1
    0xB6, 3, 0x08, 0xC2, 0x27,  // DFUNCTR
    0x11, 0 + kDelay, 120,      // SLPOUT
    0x38, 0,                    // IDMOFF
    0x29, 0 + kDelay, 100,      // DISPON
    0xFF, 0xFF,
};

// LovyanGFX's Panel_ST7735 list0 (Adafruit's "B" start-up).
constexpr uint8_t kST7735Init[] = {
    0x01, 0 + kDelay, 50,                    // SWRESET
    0x11, 0 + kDelay, 255,                   // SLPOUT, 500 ms
    0xB1, 3 + kDelay, 0x00, 0x06, 0x03, 10,  // FRMCTR1
    0xB6, 2, 0x15, 0x02,                     // DISSET5
    0xB4, 1, 0x00,                           // INVCTR: line inversion
    0xC0, 2 + kDelay, 0x02, 0x70, 10,        // PWCTR1
    0xC1, 1, 0x05,                           // PWCTR2
    0xC2, 2, 0x01, 0x02,                     // PWCTR3
    0xC5, 2 + kDelay, 0x3C, 0x38, 10,        // VMCTR1
    0xFC, 2, 0x11, 0x15,                     // PWCTR6
    0xE0, 16, 0x09, 0x16, 0x09, 0x20, 0x21, 0x1B, 0x13, 0x19, 0x17, 0x15, 0x1E, 0x2B, 0x04, 0x05, 0x02, 0x0E,  // GMCTRP1
    0xE1, 16 + kDelay, 0x0B, 0x14, 0x08, 0x1E, 0x22, 0x1D, 0x18, 0x1E, 0x1B, 0x1A, 0x24, 0x2B, 0x06, 0x06, 0x02,
    0x0F, 10,                                // GMCTRN1
    0x13, 0 + kDelay, 10,                    // NORON
    0x29, 0 + kDelay, 255,                   // DISPON, 500 ms
    0xFF, 0xFF,
};

// LovyanGFX's Panel_ST7735S lists 0 and 1, one after the other (Adafruit's
// "R" start-up, red or green tab).
constexpr uint8_t kST7735SInit[] = {
    0x01, 0 + kDelay, 150,                        // SWRESET
    0x11, 0 + kDelay, 255,                        // SLPOUT, 500 ms
    0xB1, 3, 0x01, 0x2C, 0x2D,                    // FRMCTR1
    0xB2, 3, 0x01, 0x2C, 0x2D,                    // FRMCTR2
    0xB3, 6, 0x01, 0x2C, 0x2D, 0x01, 0x2C, 0x2D,  // FRMCTR3
    0xB4, 1, 0x07,                                // INVCTR: no inversion
    0xC0, 3, 0xA2, 0x02, 0x84,                    // PWCTR1
    0xC1, 1, 0xC5,                                // PWCTR2
    0xC2, 2, 0x0A, 0x00,                          // PWCTR3
    0xC3, 2, 0x8A, 0x2A,                          // PWCTR4
    0xC4, 2, 0x8A, 0xEE,                          // PWCTR5
    0xC5, 1, 0x0E,                                // VMCTR1
    0xE0, 16, 0x02, 0x1C, 0x07, 0x12, 0x37, 0x32, 0x29, 0x2D, 0x29, 0x25, 0x2B, 0x39, 0x00, 0x01, 0x03, 0x10,  // GMCTRP1
    0xE1, 16, 0x03, 0x1D, 0x07, 0x06, 0x2E, 0x2C, 0x29, 0x2D, 0x2E, 0x2E, 0x37, 0x3F, 0x00, 0x00, 0x02, 0x10,  // GMCTRN1
    0x13, 0 + kDelay, 10,                         // NORON
    0x29, 0 + kDelay, 100,                        // DISPON
    0xFF, 0xFF,
};

// LovyanGFX's Panel_GC9A01 list0: the inter-register enables, then voltages
// and gamma, TE on, out of sleep, on.
constexpr uint8_t kGC9A01Init[] = {
    0xEF, 0,
    0xEB, 1, 0x14,
    0xFE, 0,
    0xEF, 0,
    0xEB, 1, 0x14,
    0x84, 1, 0x40,
    0x85, 1, 0xFF,
    0x86, 1, 0xFF,
    0x87, 1, 0xFF,
    0x8E, 1, 0xFF,
    0x8F, 1, 0xFF,
    0x88, 1, 0x0A,
    0x89, 1, 0x21,
    0x8A, 1, 0x00,
    0x8B, 1, 0x80,
    0x8C, 1, 0x01,
    0x8D, 1, 0x01,
    0xB6, 2, 0x00, 0x20,
    0x90, 4, 0x08, 0x08, 0x08, 0x08,
    0xBD, 1, 0x06,
    0xBC, 1, 0x00,
    0xFF, 3, 0x60, 0x01, 0x04,
    0xC3, 1, 0x13,
    0xC4, 1, 0x13,
    0xC9, 1, 0x22,
    0xBE, 1, 0x11,
    0xE1, 2, 0x10, 0x0E,
    0xDF, 3, 0x21, 0x0C, 0x02,
    0xF0, 6, 0x45, 0x09, 0x08, 0x08, 0x26, 0x2A,
    0xF1, 6, 0x43, 0x70, 0x72, 0x36, 0x37, 0x6F,
    0xF2, 6, 0x45, 0x09, 0x08, 0x08, 0x26, 0x2A,
    0xF3, 6, 0x43, 0x70, 0x72, 0x36, 0x37, 0x6F,
    0xED, 2, 0x1B, 0x0B,
    0xAE, 1, 0x77,
    0xCD, 1, 0x63,
    0x70, 9, 0x07, 0x07, 0x04, 0x0E, 0x0F, 0x09, 0x07, 0x08, 0x03,
    0xE8, 1, 0x34,
    0x62, 12, 0x18, 0x0D, 0x71, 0xED, 0x70, 0x70, 0x18, 0x0F, 0x71, 0xEF, 0x70, 0x70,
    0x63, 12, 0x18, 0x11, 0x71, 0xF1, 0x70, 0x70, 0x18, 0x13, 0x71, 0xF3, 0x70, 0x70,
    0x64, 7, 0x28, 0x29, 0xF1, 0x01, 0xF1, 0x00, 0x07,
    0x66, 10, 0x3C, 0x00, 0xCD, 0x67, 0x45, 0x45, 0x10, 0x00, 0x00, 0x00,
    0x67, 10, 0x00, 0x3C, 0x00, 0x00, 0x00, 0x01, 0x54, 0x10, 0x32, 0x98,
    0x74, 7, 0x10, 0x68, 0x80, 0x00, 0x00, 0x4E, 0x00,
    0x98, 2, 0x3E, 0x07,
    0x35, 1, 0x00,              // TE on
    0x11, 0 + kDelay, 120,      // SLPOUT
    0x29, 0,                    // DISPON
    0xFF, 0xFF,
};

// LovyanGFX's Panel_ST7789P3 list0, as it is: it sets inversion and a first
// window itself.
constexpr uint8_t kST7789P3Init[] = {
    0x11, 0 + kDelay, 120,
    0x36, 1, 0x00,
    0x3A, 1, 0x05,
    0xB2, 5, 0x0C, 0x0C, 0x00, 0x33, 0x33,
    0xB7, 1, 0x56,
    0xBB, 1, 0x1D,
    0xC0, 1, 0x2C,
    0xC2, 1, 0x01,
    0xC3, 1, 0x0F,
    0xC6, 1, 0x0F,
    0xD0, 1, 0xA7,
    0xD0, 2, 0xA4, 0xA1,
    0xD6, 1, 0xA1,
    0xE0, 14, 0xF0, 0x02, 0x07, 0x05, 0x06, 0x14, 0x2F, 0x54, 0x46, 0x38, 0x13, 0x11, 0x2E, 0x35,
    0xE1, 14, 0xF0, 0x08, 0x0C, 0x0C, 0x09, 0x05, 0x2F, 0x43, 0x46, 0x36, 0x10, 0x12, 0x2C, 0x32,
    0x21, 0,
    0x29, 0,
    0x2C, 0 + kDelay, 100,
    0x2A, 4, 0x00, 0x00, 0x01, 0x3F,
    0x2B, 4, 0x00, 0x00, 0x00, 0xEF,
    0x2C, 0,
    0xFF, 0xFF,
};

// LovyanGFX's Panel_ST7796 list0: extension commands on, timing, voltages and
// gamma, extension commands off, out of sleep, on.
constexpr uint8_t kST7796Init[] = {
    0xF0, 1, 0xC3,                                            // CSCON: extension part I on
    0xF0, 1, 0x96,                                            // CSCON: extension part II on
    0xB4, 1, 0x01,                                            // INVCTR: 1-dot inversion
    0xB6, 3, 0x80, 0x22, 0x3B,                                // DFUNCTR
    0xE8, 8, 0x40, 0x8A, 0x00, 0x00, 0x29, 0x19, 0xA5, 0x33,  // DOCA
    0xC1, 1, 0x06,                                            // PWCTR2
    0xC2, 1, 0xA7,                                            // PWCTR3
    0xC5, 1 + kDelay, 0x18, 120,                              // VMCTR: VCOM 0.9 V
    0xE0, 14, 0xF0, 0x09, 0x0B, 0x06, 0x04, 0x15, 0x2F, 0x54, 0x42, 0x3C, 0x17, 0x14, 0x18, 0x1B,  // GMCTRP1
    0xE1, 14 + kDelay, 0xE0, 0x09, 0x0B, 0x06, 0x04, 0x03, 0x2B, 0x43, 0x42, 0x3B, 0x16, 0x14, 0x17, 0x1B,
    120,                                                      // GMCTRN1
    0xF0, 1, 0x3C,                                            // CSCON: extension part I off
    0xF0, 1, 0x69,                                            // CSCON: extension part II off
    0x11, 0 + kDelay, 130,                                    // SLPOUT
    0x38, 0,                                                  // IDMOFF
    0x29, 0,                                                  // DISPON
    0xFF, 0xFF,
};

// LovyanGFX's Panel_ILI9488 list0.
constexpr uint8_t kILI9488Init[] = {
    0xC0, 2, 0x17, 0x15,              // PWCTR1: VRH1, VRH2
    0xC1, 1, 0x41,                    // PWCTR2: VGH, VGL
    0xC5, 3, 0x00, 0x12, 0x80,        // VMCTR
    0xB1, 1, 0xA0,                    // FRMCTR1: 60 Hz
    0xB4, 1, 0x02,                    // INVCTR: 2-dot
    0xB6, 3, 0x02, 0x22, 0x3B,        // DFUNCTR
    0xB7, 1, 0xC6,                    // ETMOD
    0xF7, 4, 0xA9, 0x51, 0x2C, 0x82,  // ADJCTL3
    0x11, 0 + kDelay, 120,            // SLPOUT
    0x38, 0,                          // IDMOFF
    0x29, 0 + kDelay, 100,            // DISPON
    0xFF, 0xFF,
};

// LovyanGFX's Panel_SSD1351 list0. It ends with A5 (every pixel on); the
// inversion PanelController sends after it (A6, normal) shows the memory
// again, as LovyanGFX's setInvert does.
constexpr uint8_t kSSD1351Init[] = {
    0xFD, 1, 0x12,              // CMDLOCK
    0xFD, 1, 0xB1,              // CMDLOCK
    0xAE, 0,                    // sleep
    0xB3, 1, 0xF1,              // CLOCKDIV
    0xCA, 1, 0x7F,              // MUXRATIO
    0xA2, 1, 0x00,              // DISPLAYOFFSET
    0xB5, 1, 0x00,              // SETGPIO
    0xAB, 1, 0x01,              // FUNCTIONSELECT
    0xB1, 1, 0x32,              // PRECHARGE
    0xBE, 1, 0x05,              // VCOMH
    0xA1, 1, 0x00,              // STARTLINE
    0xA6, 0,                    // normal display
    0xC1, 3, 0xC8, 0x80, 0xC8,  // CONTRASTABC
    0xC7, 1, 0x0F,              // CONTRASTMASTER
    0xB4, 3, 0xA0, 0xB5, 0x55,  // SETVSL
    0xB6, 1, 0x01,              // PRECHARGE2
    0xAF, 0,                    // wake
    0xA5, 0,
    0xFF, 0xFF,
};

// LovyanGFX's Panel_CO5300 list0: RGB565, TE on, a first window, out of sleep,
// dark until the display is on, then brightness.
constexpr uint8_t kCO5300Init[] = {
    0xFE, 1, 0x00,              // page 0
    0xC4, 1, 0x80,
    0x3A, 1, 0x55,              // COLMOD: RGB565
    0x35, 1, 0x00,              // TE on
    0x53, 1, 0x20,
    0x63, 1, 0xFF,
    0x2A, 4, 0x00, 0x16, 0x01, 0xAF,
    0x2B, 4, 0x00, 0x00, 0x01, 0xF5,
    0x11, 0 + kDelay, 0,        // SLPOUT
    0x51, 1, 0x01,              // brightness: dark
    0x29, 0 + kDelay, 0,        // DISPON
    0x51, 1, 0x80,              // brightness: on
    0xFF, 0xFF,
};

// clang-format on

// Panel_LCD::getMadCtl: rotations 0-3, then 4-7 mirrored. Most MIPI DCS
// controllers take it.
constexpr uint8_t kLcdMadctl[8] = {
    0, kMV | kMX | kMH, kMX | kMH | kMY | kML, kMV | kMY | kML, kMY | kML, kMV, kMX | kMH, kMV | kMX | kMY | kMH | kML,
};

// Panel_AMOLED::update_madctl: its own MADCTL per rotation, with no
// colour-order bit.
constexpr uint8_t kAmoledMadctl[8] = { 0x00, 0x60, 0xC0, 0xA0, 0xC2, 0x62, 0x02, 0xA2 };

// The SSD1351's commands. Its remap (A0) takes MADCTL's place.
constexpr ControllerCommands kSSD1351Commands = {
    .columns = 0x15,
    .rows = 0x75,
    .memoryWrite = 0x5C,
    .madctl = 0xA0,
    .colmod = 0,
    .invertOff = 0xA6,
    .invertOn = 0xA7,
    .sleepIn = 0xAE,
    .sleepOut = 0xAF,
    .displayOff = 0xAE,
    .displayOn = 0xAF,
    .nop = 0xE3,
    .windowBytes = 1,
    .windowTurns = true,
};

// A MIPI DCS LCD controller: Panel_LCD's MADCTL and a BGR bit.
constexpr ControllerInfo Lcd(const char* name, uint16_t memoryWidth, uint16_t memoryHeight, const uint8_t* init)
{
    ControllerInfo info;
    info.name = name;
    info.memoryWidth = memoryWidth;
    info.memoryHeight = memoryHeight;
    info.defaultInit = init;
    for (int r = 0; r < 8; ++r)
    {
        info.madctl[r] = kLcdMadctl[r];
    }
    return info;
}

// A Panel_AMOLED controller: quad SPI only, offsets applied as they are, rows
// in pairs (it ignores a write whose start or size is odd along its short
// axis), no NOP, and brightness by command, having no backlight.
constexpr ControllerInfo Amoled(const char* name, uint16_t memoryWidth, uint16_t memoryHeight, const uint8_t* init,
                                uint8_t onBrightness)
{
    ControllerInfo info;
    info.name = name;
    info.memoryWidth = memoryWidth;
    info.memoryHeight = memoryHeight;
    info.defaultInit = init;
    for (int r = 0; r < 8; ++r)
    {
        info.madctl[r] = kAmoledMadctl[r];
    }
    info.bgrBit = 0;
    info.nopOnEnd = false;
    info.rowAlign = 2;
    info.offsetsFollowRotation = false;
    info.brightnessCmd = 0x51;
    info.onBrightness = onBrightness;
    return info;
}

// Panel_GC9xxx: its own MADCTL, no NOP (it misbehaves on one), and rows
// opened before a turned window's columns.
constexpr ControllerInfo GC9A01()
{
    ControllerInfo info;
    info.name = "GC9A01";
    info.memoryWidth = 240;
    info.memoryHeight = 240;
    info.defaultInit = kGC9A01Init;
    constexpr uint8_t madctl[8] = { 0, kMV | kMX, kMX | kMY, kMV | kMY, kMY, kMV, kMX, kMV | kMX | kMY };
    for (int r = 0; r < 8; ++r)
    {
        info.madctl[r] = madctl[r];
    }
    info.nopOnEnd = false;
    info.openRowsBeforeColumns = true;
    return info;
}

// Panel_SSD1351: its own commands, one byte per window coordinate, the window
// turned with the panel, and its remap per rotation with 0x40 for 65k colours
// (Panel_SSD1351::update_madctl); 0x04 is its colour-order bit.
constexpr ControllerInfo SSD1351()
{
    ControllerInfo info;
    info.name = "SSD1351";
    info.memoryWidth = 128;
    info.memoryHeight = 128;
    info.defaultInit = kSSD1351Init;
    constexpr uint8_t remap[8] = { 0x60, 0x63, 0x72, 0x71, 0x70, 0x61, 0x62, 0x73 };
    for (int r = 0; r < 8; ++r)
    {
        info.madctl[r] = remap[r];
    }
    info.bgrBit = 0x04;
    info.commands = kSSD1351Commands;
    return info;
}

// In Controller's order.
constexpr ControllerInfo kControllers[] = {
    Lcd("ST7789", 240, 320, kST7789Init),
    Amoled("RM67162", 240, 536, kRM67162Init, 0xAF),
    Lcd("ILI9341", 240, 320, kILI9341Init),
    // 132x162, 128x160 or 132x132, by the module's mode pins.
    Lcd("ST7735", 132, 162, kST7735Init),
    Lcd("ST7735S", 132, 162, kST7735SInit),
    GC9A01(),
    Lcd("ST7789P3", 240, 320, kST7789P3Init),
    Lcd("ST7796", 320, 480, kST7796Init),
    []
    {
        ControllerInfo info = Lcd("ILI9488", 320, 480, kILI9488Init);
        info.spiNeeds18Bits = true;
        return info;
    }(),
    SSD1351(),
    // As LovyanGFX's Panel_CO5300 has it.
    Amoled("CO5300", 502, 410, kCO5300Init, 0x80),
};

}  // namespace

const ControllerInfo& GetControllerInfo(Controller c)
{
    const auto i = static_cast<size_t>(c);
    return i < sizeof(kControllers) / sizeof(kControllers[0]) ? kControllers[i] : kControllers[0];
}

}  // namespace DekiDisplay
