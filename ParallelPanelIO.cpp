#include "ParallelPanelIO.h"

#include <deki/LogSystem.h>

namespace DekiDisplay
{

ParallelBus::Factory ParallelBus::s_Factory = nullptr;

void ParallelBus::SetFactory(Factory factory)
{
    s_Factory = factory;
}

bool ParallelBus::HasFactory()
{
    return s_Factory != nullptr;
}

std::unique_ptr<IParallelBus> ParallelBus::Create(const ParallelBusConfig& config)
{
    if (!s_Factory)
    {
        DEKI_LOG_ERROR("ParallelBus: no factory registered - the platform package must call SetFactory()");
        return nullptr;
    }
    return s_Factory(config);
}

bool ParallelPanelIO::Begin()
{
    ++m_Depth;
    return true;
}

void ParallelPanelIO::End()
{
    if (m_Depth > 0 && --m_Depth == 0)
    {
        m_Bus.Wait(0);
    }
}

bool ParallelPanelIO::Command(uint8_t cmd, const uint8_t* params, size_t n)
{
    m_PendingCmd = -1;
    return m_Bus.Command(cmd, params, n);
}

bool ParallelPanelIO::StartPixels(uint8_t memoryWriteCmd)
{
    m_PendingCmd = memoryWriteCmd;
    return true;
}

bool ParallelPanelIO::QueuePixels(const void* data, size_t bytes, bool /*more*/)
{
    const bool ok = m_Bus.QueuePixels(m_PendingCmd, data, bytes);
    m_PendingCmd = -1;
    ++m_Queued;
    return ok;
}

bool ParallelPanelIO::PixelsBigEndian() const
{
    // Sixteen lines take a pixel's low byte on D0-D7, the first byte in memory.
    return m_Bus.GetWidth() != 16;
}

}  // namespace DekiDisplay
