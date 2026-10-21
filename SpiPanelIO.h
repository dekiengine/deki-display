#pragma once

#include "IPanelIO.h"
#include "IDekiSPI.h"  // from deki-spi

namespace DekiDisplay
{

/// A panel on 4-wire SPI (MIPI DBI type C): the DC pin low for a command
/// byte, high for its parameters and for pixels. Pixels go out as queued DMA
/// transfers on the device, with chip select held low from the memory-write
/// command to the last part, so a write sent in pieces is one write to the
/// panel.
class SpiPanelIO : public IPanelIO
{
public:
    /// `device` is a half-duplex device with the panel's DC pin, from
    /// IDekiSPIBus::AddDevice. The bus keeps it; this only borrows it.
    explicit SpiPanelIO(DekiSpi::IDekiSPIDevice* device) : m_Device(device) {}

    bool Begin() override;
    void End() override;
    bool Command(uint8_t cmd, const uint8_t* params, size_t n) override;
    bool StartPixels(uint8_t memoryWriteCmd) override;
    bool QueuePixels(const void* data, size_t bytes, bool more) override;
    bool WaitPixels(size_t leaveInFlight) override;
    size_t GetMaxChunkBytes() const override;
    bool PixelsBigEndian() const override { return true; }

private:
    DekiSpi::IDekiSPIDevice* m_Device;
    int m_Depth = 0;
};

}  // namespace DekiDisplay
