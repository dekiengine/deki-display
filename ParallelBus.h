#pragma once

#include "DekiDisplayPackage.h"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace DekiDisplay
{

/// How a panel on an 8080-style parallel bus (MIPI DBI type B) is wired.
struct ParallelBusConfig
{
    /// Data lines: 8 or 16.
    uint8_t width = 8;
    /// D0 first; the first `width` are used.
    int32_t dataPins[16] = { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 };
    /// Write strobe: the panel takes each byte or word on its edge.
    int32_t wrPin = -1;
    /// Read strobe, held high: the panel is only written. -1 when it is tied
    /// high on the board.
    int32_t rdPin = -1;
    /// Data/command (RS): low for a command, high for its parameters and
    /// for pixels.
    int32_t dcPin = -1;
    /// -1 when chip select is tied low.
    int32_t csPin = -1;
    /// Write strobe frequency.
    uint32_t clockHz = 20000000;
    /// The longest pixel transfer.
    size_t maxTransferBytes = 32768;
    /// Pixel transfers that may be queued at once.
    uint8_t queueDepth = 8;
};

/// A panel's parallel bus, as the platform drives it: commands, and pixels
/// queued by DMA.
///
/// On a 16-bit bus, a command and each of its parameters take one cycle, the
/// byte on D0-D7, and a pixel is one word, its first byte in memory on D0-D7.
class IParallelBus
{
public:
    virtual ~IParallelBus() = default;

    /// Sends `cmd`, then its `n` parameter bytes, once the queued pixels are
    /// sent.
    virtual bool Command(uint8_t cmd, const uint8_t* params, size_t n) = 0;
    /// Queues `bytes` of pixels, after the command `cmd` that opens the
    /// write, or continuing the write when `cmd` is -1. `data` must stay valid
    /// until Wait has returned for it, and be no longer than
    /// GetMaxTransferBytes.
    virtual bool QueuePixels(int32_t cmd, const void* data, size_t bytes) = 0;
    /// Waits until no more than `leaveInFlight` queued pixel transfers are
    /// still being sent.
    virtual bool Wait(size_t leaveInFlight) = 0;
    virtual size_t GetMaxTransferBytes() const = 0;
    /// Data lines: 8 or 16.
    virtual uint8_t GetWidth() const = 0;
};

/// Makes parallel panel buses: the platform package registers a factory, and
/// DisplayPanelComponent asks it for the bus its panel is wired to.
class DEKI_DISPLAY_API ParallelBus
{
public:
    /// A bus for `config`, or nullptr when it cannot be started (the factory
    /// logs why).
    using Factory = std::unique_ptr<IParallelBus> (*)(const ParallelBusConfig& config);

    static void SetFactory(Factory factory);
    static bool HasFactory();
    /// The factory's bus for `config`; nullptr without a factory.
    static std::unique_ptr<IParallelBus> Create(const ParallelBusConfig& config);

private:
    static Factory s_Factory;
};

}  // namespace DekiDisplay
