// Portions derived from LovyanGFX (https://github.com/lovyan03/LovyanGFX),
// Copyright (c) lovyan03 and contributors, FreeBSD licence: the quad-SPI
// framing of Panel_AMOLED (write_cmd, start_qspi). See NOTICE.

#include "QspiPanelIO.h"

namespace DekiDisplay
{

bool QspiPanelIO::Begin()
{
    if (!m_Device || !m_Device->Acquire())
    {
        return false;
    }
    ++m_Depth;
    return true;
}

void QspiPanelIO::End()
{
    if (m_Depth == 0)
    {
        return;
    }
    if (--m_Depth == 0)
    {
        m_Device->Wait(0);
    }
    m_Device->Release();
}

bool QspiPanelIO::Queue(const DekiSpi::Transfer& t)
{
    if (!m_Device->Queue(t))
    {
        return false;
    }
    ++m_Queued;
    return true;
}

bool QspiPanelIO::Command(uint8_t cmd, const uint8_t* params, size_t n)
{
    // Inside a run, behind the pixels before it; the device copies four
    // bytes or less, so the parameters need not outlive this call.
    const bool queue = m_Depth > 0 && n <= 4;
    if (!Begin())
    {
        return false;
    }
    DekiSpi::Transfer t;
    t.cmdBits = 8;
    t.cmd = kRegisterWrite;
    t.addrBits = 24;
    t.addr = static_cast<uint32_t>(cmd) << 8;
    t.tx = params;
    t.txBytes = n;
    const bool ok = queue ? Queue(t) : m_Device->Transmit(t);
    End();
    return ok;
}

bool QspiPanelIO::StartPixels(uint8_t memoryWriteCmd)
{
    // Sent with the first part of the pixels: the phases and the data are one
    // transfer.
    m_WriteOpening = true;
    m_MemoryWriteCmd = memoryWriteCmd;
    return m_Device != nullptr;
}

bool QspiPanelIO::QueuePixels(const void* data, size_t bytes, bool more)
{
    if (!m_Device || bytes == 0)
    {
        return bytes == 0;
    }
    DekiSpi::Transfer t;
    t.tx = data;
    t.txBytes = bytes;
    t.lines = DekiSpi::Lines::Quad;
    if (m_WriteOpening)
    {
        t.cmdBits = 8;
        t.cmd = kPixelWriteQuad;
        t.addrBits = 24;
        t.addr = static_cast<uint32_t>(m_MemoryWriteCmd) << 8;
        m_WriteOpening = false;
    }
    // Chip select stays low into the next part, which continues the write.
    t.keepCs = more && m_Depth > 0;
    return Queue(t);
}

bool QspiPanelIO::WaitPixels(size_t leaveInFlight)
{
    return m_Device && m_Device->Wait(leaveInFlight);
}

size_t QspiPanelIO::GetMaxChunkBytes() const
{
    return m_Device ? m_Device->GetMaxTransferBytes() : 0;
}

}  // namespace DekiDisplay
