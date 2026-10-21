#pragma once

#include "IPanelIO.h"
#include "IDekiSPI.h"  // from deki-spi

namespace DekiDisplay
{

/// A panel on quad SPI, as AMOLED controllers (RM67162, SH8601, CO5300) take
/// it: no DC pin, the kind of write named in front of each transfer instead.
///
/// - A command is a register write: command phase 0x02, then the register in
///   the middle byte of a 24-bit address, then its parameters, all on one
///   line; chip select goes high after each.
/// - Pixels are a memory write on four lines: command phase 0x32, the
///   memory-write command in the address, then the pixels. Chip select stays
///   low from there to the last part, so parts after the first carry no
///   phases and continue the same write.
///
/// The framing is LovyanGFX's (Panel_AMOLED: write_cmd, start_qspi), sent
/// through the bus's own command and address phases.
class QspiPanelIO : public IPanelIO
{
public:
    /// `device` is a half-duplex device on a bus with IO2 and IO3 wired,
    /// from IDekiSPIBus::AddDevice. The bus keeps it; this only borrows it.
    explicit QspiPanelIO(DekiSpi::IDekiSPIDevice* device) : m_Device(device) {}

    static constexpr uint8_t kRegisterWrite = 0x02;
    static constexpr uint8_t kPixelWriteQuad = 0x32;

    bool Begin() override;
    void End() override;
    bool Command(uint8_t cmd, const uint8_t* params, size_t n) override;
    bool StartPixels(uint8_t memoryWriteCmd) override;
    bool QueuePixels(const void* data, size_t bytes, bool more) override;
    bool WaitPixels(size_t leaveInFlight) override;
    uint32_t GetQueuedCount() const override { return m_Queued; }
    size_t GetMaxChunkBytes() const override;
    bool PixelsBigEndian() const override { return true; }

private:
    bool Queue(const DekiSpi::Transfer& t);

    DekiSpi::IDekiSPIDevice* m_Device;
    int m_Depth = 0;
    uint32_t m_Queued = 0;
    // StartPixels names the write; its first part carries the phases.
    bool m_WriteOpening = false;
    uint8_t m_MemoryWriteCmd = 0;
};

}  // namespace DekiDisplay
