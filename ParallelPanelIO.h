#pragma once

#include "IPanelIO.h"
#include "ParallelBus.h"

namespace DekiDisplay
{

/// A panel on an 8080-style parallel bus (MIPI DBI type B), 8 or 16 data
/// lines and a DC pin. The memory-write command goes out with the first part
/// of the pixels and the parts after it continue the write.
///
/// On 8 lines a pixel goes high byte first, as on SPI. On 16 it is one word,
/// its low byte (the first in memory) on D0-D7: a little-endian frame goes out
/// with no swap.
class ParallelPanelIO : public IPanelIO
{
public:
    /// `bus` must outlive this.
    explicit ParallelPanelIO(IParallelBus& bus)
        : m_Bus(bus)
    {
    }

    bool Begin() override;
    void End() override;
    /// Sent once the queued pixels are: the bus has nothing to queue it behind.
    bool Command(uint8_t cmd, const uint8_t* params, size_t n) override;
    bool StartPixels(uint8_t memoryWriteCmd) override;
    bool QueuePixels(const void* data, size_t bytes, bool more) override;
    bool WaitPixels(size_t leaveInFlight) override { return m_Bus.Wait(leaveInFlight); }
    uint32_t GetQueuedCount() const override { return m_Queued; }
    size_t GetMaxChunkBytes() const override { return m_Bus.GetMaxTransferBytes(); }
    /// High byte first on 8 data lines; as in memory on 16.
    bool PixelsBigEndian() const override;

private:
    IParallelBus& m_Bus;
    int m_Depth = 0;
    // The memory write that opens the next part, or -1 to continue one.
    int32_t m_PendingCmd = -1;
    uint32_t m_Queued = 0;
};

}  // namespace DekiDisplay
