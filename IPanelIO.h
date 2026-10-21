#pragma once

#include <cstddef>
#include <cstdint>

namespace DekiDisplay
{

/// What a panel controller speaks: commands with their parameters, and a
/// stream of pixels into the panel's memory. One implementation per kind of
/// bus (SpiPanelIO for 4-wire SPI), so a controller never knows how its bytes
/// travel.
///
/// Pixels are queued and go out by DMA while the caller fills the next
/// buffer: QueuePixels returns before they are sent, and WaitPixels says when
/// a buffer is free again. A command waits for the pixels before it.
class IPanelIO
{
public:
    virtual ~IPanelIO() = default;

    /// Holds the bus for a run of commands and pixels (a frame, a strip).
    /// Calls nest; Command and StartPixels hold it themselves when they are
    /// called outside a Begin.
    virtual bool Begin() = 0;
    /// Waits for the pixels, then lets the bus go to its other devices.
    virtual void End() = 0;

    /// Sends `cmd`, then its `n` parameters. Waits for queued pixels first.
    virtual bool Command(uint8_t cmd, const uint8_t* params, size_t n) = 0;

    /// Opens a write into the panel's memory with `memoryWriteCmd` (RAMWR,
    /// 0x2C, on a MIPI DCS controller); the window was set before it.
    virtual bool StartPixels(uint8_t memoryWriteCmd) = 0;
    /// Queues the next `bytes` of the open write, in the panel's byte order.
    /// `more` says another queued part follows, so the write is not closed.
    /// `data` must stay valid until WaitPixels has returned for it, and be
    /// no longer than GetMaxChunkBytes.
    virtual bool QueuePixels(const void* data, size_t bytes, bool more) = 0;
    /// Waits until no more than `leaveInFlight` queued parts are still being
    /// sent: 1 frees the older of two alternating buffers, 0 waits for all.
    virtual bool WaitPixels(size_t leaveInFlight) = 0;

    /// The longest part QueuePixels takes.
    virtual size_t GetMaxChunkBytes() const = 0;
    /// True when the wire takes RGB565 high byte first, as every serial
    /// panel does. A frame in the other order is swapped on its way out.
    virtual bool PixelsBigEndian() const = 0;
};

}  // namespace DekiDisplay
