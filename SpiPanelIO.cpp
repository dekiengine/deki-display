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
    // Only the outermost End waits: a Begin inside a held run (a strip inside
    // the worker's run of strips) leaves what it queued on the wire.
    if (--m_Depth == 0)
    {
        m_Device->Wait(0);
    }
    m_Device->Release();
}

bool SpiPanelIO::Queue(const DekiSpi::Transfer& t)
{
    if (!m_Device->Queue(t))
    {
        return false;
    }
    ++m_Queued;
    return true;
}

bool SpiPanelIO::Command(uint8_t cmd, const uint8_t* params, size_t n)
{
    // Inside a run, behind the pixels before it: the device copies a transfer
    // of four bytes or less, so neither needs to outlive this call.
    const bool queue = m_Depth > 0 && n <= 4;
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
    bool ok = queue ? Queue(c) : m_Device->Transmit(c);
    if (ok && n > 0)
    {
        DekiSpi::Transfer p;
        p.tx = params;
        p.txBytes = n;
        p.dc = 1;
        ok = queue ? Queue(p) : m_Device->Transmit(p);
    }
    End();
    return ok;
}

bool SpiPanelIO::StartPixels(uint8_t memoryWriteCmd)
{
    if (!m_Device)
    {
        return false;
    }
    // Inside a Begin the command is queued, and keeps chip select low into
    // the pixels that follow it. Outside one there is no bus to hold, and the
    // panel takes the pixels after chip select has gone high and low again,
    // as a MIPI DBI controller does until the next command.
    DekiSpi::Transfer c;
    c.tx = &memoryWriteCmd;
    c.txBytes = 1;
    c.dc = 0;
    c.keepCs = m_Depth > 0;
    return m_Depth > 0 ? Queue(c) : m_Device->Transmit(c);
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
    return Queue(t);
}

bool SpiPanelIO::WaitPixels(size_t leaveInFlight)
{
    return m_Device && m_Device->Wait(leaveInFlight);
}

size_t SpiPanelIO::GetMaxChunkBytes() const
{
    return m_Device ? m_Device->GetMaxTransferBytes() : 0;
}

bool SpiPanelIO::Read(uint8_t cmd, uint8_t* out, size_t n)
{
    // One transfer, the command as its command phase: a device read at a
    // clock of its own reads through a second driver handle, and the whole
    // read is on it.
    DekiSpi::Transfer r;
    r.cmdBits = 8;
    r.cmd = cmd;
    r.rx = out;
    r.rxBytes = n;
    r.dc = 0;
    return m_Device->Transmit(r);
}

}  // namespace DekiDisplay
