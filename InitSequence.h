#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace DekiDisplay
{

/// One command of a panel's start-up: the command byte, its parameters, and
/// how long to wait after it.
struct InitStep
{
    uint8_t cmd = 0;
    std::vector<uint8_t> params;
    uint32_t delayMs = 0;
};

/// The commands that start a panel, sent once after reset. A controller has
/// a default; a board's boot scene can replace it with its own text, because
/// each panel module needs its own voltages, VCOM and gamma.
using InitSequence = std::vector<InitStep>;

/// Reads a table in LovyanGFX's format, which the controller defaults are
/// ported in: command, count of parameters (0x80 added when a delay follows),
/// the parameters, the delay in ms (255 meaning 500), and 0xFF 0xFF to end.
InitSequence InitSequenceFromTable(const uint8_t* table);

/// Reads the text form a boot scene holds:
///
///     # comment
///     11; delay 120          # SLPOUT, then wait 120 ms
///     B2 0C 0C 00 33 33      # PORCTRL and its five parameters
///     BB 19
///
/// A command is its hex byte followed by its parameter bytes (0x is
/// optional), one per line or separated by ';'. `delay <ms>` waits after the
/// command before it, on its own or at the end of that command. False, with
/// `error` naming the line, when the text is not that.
bool ParseInitSequence(const std::string& text, InitSequence& out, std::string& error);

}  // namespace DekiDisplay
