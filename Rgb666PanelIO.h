#pragma once

#include "IPanelIO.h"

#include <cstddef>
#include <cstdint>

namespace DekiDisplay
{

/// Sends pixels as RGB666, three bytes each, through another panel IO: for a
/// controller that takes only 18 bits a pixel on its bus (the ILI9488 over
/// SPI). Commands pass through as they are.
///
/// Pixels arrive as RGB565, high byte first, and are widened into this IO's
/// own DMA buffers, a piece at a time, while the inner IO sends the pieces
/// before. A caller's buffer is free again as soon as QueuePixels returns, so
/// WaitPixels only has to wait when asked for everything.
class Rgb666PanelIO : public IPanelIO
{
public:
    /// Pieces widened at once: one is filled while the others are sent.
    static constexpr int kBuffers = 3;

    /// `inner` sends the widened pixels; it must outlive this.
    explicit Rgb666PanelIO(IPanelIO& inner);
    ~Rgb666PanelIO() override;

    Rgb666PanelIO(const Rgb666PanelIO&) = delete;
    Rgb666PanelIO& operator=(const Rgb666PanelIO&) = delete;

    bool Begin() override { return m_Inner.Begin(); }
    void End() override { m_Inner.End(); }
    bool Command(uint8_t cmd, const uint8_t* params, size_t n) override { return m_Inner.Command(cmd, params, n); }
    bool StartPixels(uint8_t memoryWriteCmd) override { return m_Inner.StartPixels(memoryWriteCmd); }
    bool QueuePixels(const void* data, size_t bytes, bool more) override;
    bool WaitPixels(size_t leaveInFlight) override;
    uint32_t GetQueuedCount() const override { return m_Queued; }
    /// Two thirds of the inner IO's, so a band of RGB565 widens to one of its
    /// transfers.
    size_t GetMaxChunkBytes() const override;
    bool PixelsBigEndian() const override { return true; }

    /// Widens `pixels` RGB565 pixels, high byte first, to RGB666 in `out`:
    /// each channel in a byte's top bits, its high bits repeated below them.
    static void Widen(const uint8_t* rgb565, size_t pixels, uint8_t* out);

private:
    // Allocates the buffers on first use. False when there is no memory.
    bool EnsureBuffers();

    IPanelIO& m_Inner;
    uint8_t* m_Buffers[kBuffers] = {};
    size_t m_BufferPixels = 0;
    // The inner IO's queued count after each buffer was queued, so a buffer
    // is reused only once its piece is on the wire.
    uint32_t m_QueuedAt[kBuffers] = {};
    bool m_InFlight[kBuffers] = {};
    int m_Next = 0;
    uint32_t m_Queued = 0;
};

}  // namespace DekiDisplay
