#include "SpiPanelIO.h"

namespace DekiDisplay
{

bool SpiPanelIO::Begin()
{
    if (!m_Device || !m_Device->Acquire())
    {
        return false;
    }
    ++m_Depth;
    return true;
}

void SpiPanelIO::End()
{
    if (m_Depth == 0)
    {
        return;
    }
    --m_Depth;
    m_Device->Wait(0);
    m_Device->Release();
}

bool SpiPanelIO::Command(uint8_t cmd, const uint8_t* params, size_t n)
{
    if (!Begin())
    {
        return false;
    }
    // Chip select stays low from the command byte into its parameters.
    DekiSpi::Transfer c;
    c.tx = &cmd;
    c.txBytes = 1;
    c.dc = 0;
    c.keepCs = n > 0;
    bool ok = m_Device->Transmit(c);
    if (ok && n > 0)
    {
        DekiSpi::Transfer p;
        p.tx = params;
        p.txBytes = n;
        p.dc = 1;
        ok = m_Device->Transmit(p);
    }
    End();
    return ok;
}

bool SpiPanelIO::StartPixels(uint8_t memoryWriteCmd)
{
    // Inside a Begin the command keeps chip select low into the pixels that
    // follow it. Outside one there is no bus to hold, and the panel takes the
    // pixels after chip select has gone high and low again, as a MIPI DBI
    // controller does until the next command.
    DekiSpi::Transfer c;
    c.tx = &memoryWriteCmd;
    c.txBytes = 1;
    c.dc = 0;
    c.keepCs = m_Depth > 0;
    return m_Device && m_Device->Transmit(c);
}

bool SpiPanelIO::QueuePixels(const void* data, size_t bytes, bool more)
{
    if (!m_Device || bytes == 0)
    {
        return bytes == 0;
    }
    DekiSpi::Transfer t;
    t.tx = data;
    t.txBytes = bytes;
    t.dc = 1;
    t.keepCs = more && m_Depth > 0;
    return m_Device->Queue(t);
}

bool SpiPanelIO::WaitPixels(size_t leaveInFlight)
{
    return m_Device && m_Device->Wait(leaveInFlight);
}

size_t SpiPanelIO::GetMaxChunkBytes() const
{
    return m_Device ? m_Device->GetMaxTransferBytes() : 0;
}

}  // namespace DekiDisplay
