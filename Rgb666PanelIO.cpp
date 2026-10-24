#include "Rgb666PanelIO.h"

#include <deki/LogSystem.h>
#include <deki/providers/Memory.h>

#include <algorithm>

namespace DekiDisplay
{

namespace
{
// A piece is at most this many pixels: 6 KB of internal DMA memory each.
constexpr size_t kMaxPiecePixels = 2048;
}  // namespace

Rgb666PanelIO::Rgb666PanelIO(IPanelIO& inner)
    : m_Inner(inner)
{
}

Rgb666PanelIO::~Rgb666PanelIO()
{
    m_Inner.WaitPixels(0);
    for (uint8_t*& buffer : m_Buffers)
    {
        Deki::Memory::Free(buffer);
        buffer = nullptr;
    }
}

size_t Rgb666PanelIO::GetMaxChunkBytes() const
{
    return std::max<size_t>(2, m_Inner.GetMaxChunkBytes() / 3 * 2);
}

void Rgb666PanelIO::Widen(const uint8_t* rgb565, size_t pixels, uint8_t* out)
{
    for (size_t i = 0; i < pixels; ++i, rgb565 += 2, out += 3)
    {
        const uint32_t v = (static_cast<uint32_t>(rgb565[0]) << 8) | rgb565[1];
        const uint32_t r = v >> 11;
        const uint32_t g = (v >> 5) & 0x3F;
        const uint32_t b = v & 0x1F;
        out[0] = static_cast<uint8_t>((r << 3) | (r >> 2));
        out[1] = static_cast<uint8_t>((g << 2) | (g >> 4));
        out[2] = static_cast<uint8_t>((b << 3) | (b >> 2));
    }
}

bool Rgb666PanelIO::EnsureBuffers()
{
    if (m_Buffers[0])
    {
        return true;
    }
    m_BufferPixels = std::clamp<size_t>(m_Inner.GetMaxChunkBytes() / 3, 1, kMaxPiecePixels);
    for (uint8_t*& buffer : m_Buffers)
    {
        buffer = static_cast<uint8_t*>(Deki::Memory::AllocateDma(m_BufferPixels * 3, Deki::Memory::Internal));
        if (!buffer)
        {
            DEKI_LOG_ERROR("Rgb666PanelIO: no internal DMA memory for %u bytes of RGB666 pixels",
                           static_cast<unsigned>(m_BufferPixels * 3));
            for (uint8_t*& b : m_Buffers)
            {
                Deki::Memory::Free(b);
                b = nullptr;
            }
            return false;
        }
    }
    return true;
}

bool Rgb666PanelIO::QueuePixels(const void* data, size_t bytes, bool more)
{
    if (!EnsureBuffers())
    {
        return false;
    }
    const auto* src = static_cast<const uint8_t*>(data);
    size_t pixels = bytes / 2;
    bool ok = true;
    while (pixels > 0 && ok)
    {
        // The buffer's last piece must be on the wire before it is filled again.
        const int i = m_Next;
        if (m_InFlight[i])
        {
            m_Inner.WaitPixels(m_Inner.GetQueuedCount() - m_QueuedAt[i]);
        }
        const size_t n = std::min(pixels, m_BufferPixels);
        Widen(src, n, m_Buffers[i]);
        src += n * 2;
        pixels -= n;
        ok = m_Inner.QueuePixels(m_Buffers[i], n * 3, more || pixels > 0);
        m_QueuedAt[i] = m_Inner.GetQueuedCount();
        m_InFlight[i] = ok;
        m_Next = (i + 1) % kBuffers;
    }
    ++m_Queued;
    return ok;
}

bool Rgb666PanelIO::WaitPixels(size_t leaveInFlight)
{
    // The caller's pixels were copied as they were queued; only "all of them"
    // needs the wire.
    if (leaveInFlight > 0)
    {
        return true;
    }
    const bool ok = m_Inner.WaitPixels(0);
    for (bool& inFlight : m_InFlight)
    {
        inFlight = false;
    }
    return ok;
}

}  // namespace DekiDisplay
