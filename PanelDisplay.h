#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

#include <deki/Engine.h>  // ColorFormat
#include <deki/providers/IDisplay.h>

#include "IPanelIO.h"
#include "PanelController.h"

namespace DekiDisplay
{

/// Deki::IDisplay on a panel this package drives: presents the engine's
/// frames, or the rectangles of them that changed, through a PanelController
/// and its IPanelIO, with the UI overlay blended over what is sent.
///
/// Rows go out through two small DMA-capable bands: a band is converted to
/// RGB565 in the panel's byte order (and the overlay blended over it) while
/// the band before it is on the wire, so the engine's framebuffer is never
/// changed. One window covers each run of rows, and the bands stream into
/// it.
class PanelDisplay : public Deki::IDisplay
{
public:
    struct Options
    {
        /// Offer a render buffer in external memory (PSRAM), freeing a
        /// screen's worth of internal RAM; drawing into it is slower.
        bool usePsram = false;
        /// Two render buffers, swapped after each present.
        bool doubleBuffer = false;
        /// Rows per band. 16 rows of 320 (10 KB) sent fastest on a T-Deck
        /// Plus: the first band's conversion is the only one not hidden
        /// behind a transfer.
        int32_t bandRows = 16;
        /// The backlight's GPIO, -1 for none (an AMOLED has none).
        int32_t backlightPin = -1;
        /// The backlight is on when the pin is low.
        bool backlightInvert = false;
    };

    PanelDisplay(PanelController& controller, IPanelIO& io);
    ~PanelDisplay() override;

    /// Allocates the bands (and render buffers) for the controller's rotated
    /// size, clears the panel to black, then turns the backlight on.
    bool Start(const Options& options);

    bool Initialize(int32_t width, int32_t height) override;
    void Shutdown() override;
    void Present(const uint8_t* framebuffer, int width, int height, Deki::ColorFormat format) override;
    bool SupportsPartialPresent() const override { return true; }
    void PresentRegions(const uint8_t* framebuffer, int width, int height, Deki::ColorFormat format,
                        const Deki::Rect* rects, int32_t count) override;
    void GetDisplaySize(int32_t* width, int32_t* height) const override;
    // Every panel here takes RGB565, swapped on the wire when it asks.
    Deki::ColorFormat GetColorFormat() const override { return Deki::ColorFormat::RGB565; }
    bool IsInitialized() const override { return m_Initialized; }
    void RequestFullRefresh() override {}
    bool ProcessEvents() override { return true; }

    void* CreateUIOverlay(int32_t width, int32_t height) override;
    bool UpdateUIOverlay(void* overlay, int32_t x, int32_t y, int32_t width, int32_t height,
                         const uint32_t* pixels) override;
    bool UpdateUIOverlayRGB565A8(void* overlay, int32_t x, int32_t y, int32_t width, int32_t height,
                                 const uint8_t* rgb565a8Pixels) override;
    void DestroyUIOverlay(void* overlay) override;
    void SetActiveUIOverlay(void* overlay) override;
    void ClearActiveUIOverlay() override;
    uint8_t* GetRenderBuffer(int32_t* width, int32_t* height) override;
    void SetBacklight(bool on) override;

    /// Sends a raw command to the panel between presents, for tuning one
    /// live (VCOM, gamma).
    bool SendCommand(uint8_t cmd, const uint8_t* params, size_t n);

    /// How a platform runs the present worker on the core the engine is not
    /// on. The ESP32 package registers a FreeRTOS task pinned there; with no
    /// backend, frames are sent as they are drawn.
    struct WorkerBackend
    {
        /// Starts `run(arg)` on the other core. `run` returns when the worker
        /// is stopped; the backend ends the task or thread then.
        bool (*start)(void (*run)(void* arg), void* arg);
        /// Lets the other core's idle task run: at full speed the next job is
        /// always waiting, and the worker would never block.
        void (*yield)();
    };
    static void SetWorkerBackend(const WorkerBackend* backend);

    /// Sends each frame from the other core while the next is drawn, so a
    /// frame costs the longer of drawing and sending rather than both:
    ///
    /// - A whole frame is copied, in the wire's byte order, to one of two
    ///   copies in external DMA memory (the overlay blended over the copy),
    ///   and the engine draws on; the worker sends the copy by DMA as it is.
    ///   The engine waits only when both copies are still being sent.
    /// - A strip (PresentRegions with one rectangle) is copied with one
    ///   memcpy to one of two copies in internal DMA memory, of up to
    ///   kMaxStripBytes each, and sent from there; the byte swap is done in
    ///   place on the worker, a chunk at a time while the chunk before it is
    ///   on the wire.
    ///
    /// Every other present waits for the worker first. Needs a worker backend
    /// and the external memory for two frames; without them it stays off.
    /// With render buffers (usePsram, doubleBuffer) too: the worker sends a
    /// copy, so the engine drawing into its buffer never touches what is on
    /// the wire.
    bool SetAsyncPresent(bool on);
    bool IsAsyncPresent() const { return m_Async; }
    /// Waits until the worker has sent every frame and strip it was given.
    void WaitForPresents();

    static constexpr size_t kMaxStripBytes = 64 * 1024;

private:
    // What the worker sends: rows [top, bottom) of the frame in
    // m_Staging[index], or, for a strip, the rows held from the start of
    // m_Strip[index]; or the order to stop.
    struct Job
    {
        int index;
        int top;
        int bottom;
        bool strip;
        bool stop;
    };
    static void WorkerMain(void* self);
    void RunWorker();
    void Post(const Job& job);
    // Gives the worker's last job's copy back to the drawing core once its
    // parts are sent. Called as soon as the next job has queued a part
    // behind them, so the wire stays busy while it waits.
    void GiveBackPendingCopy();
    // A free copy's index, after waiting for one. Strips and frames share
    // the count and alternate their indices, so the copy a job takes is
    // always one whose last job has finished.
    int TakeCopy();
    bool CanPresentAsync(const uint8_t* framebuffer, int width, int height, Deki::ColorFormat format) const;
    // Copies rows [top, bottom) of the frame for the worker and queues them.
    // False when this present cannot go that way.
    bool PresentRowsAsync(const uint8_t* framebuffer, int width, int height, Deki::ColorFormat format, int top,
                          int bottom);
    bool EnsureStrips(int rows);
    void FreeStrips();
    // Queues a strip copy, swapped in place a chunk at a time, inside the
    // worker's held write.
    void SendStripBody(uint16_t* strip, int top, int rows);

    struct UIOverlay
    {
        uint32_t* buffer;  // ARGB8888
        int32_t width;
        int32_t height;
        // Per row, the pixels [left, right) outside which the row is fully
        // transparent (left >= right: all of it), so a blend reads only those
        // from external memory.
        std::vector<uint16_t> spanLeft;
        std::vector<uint16_t> spanRight;
    };

    bool EnsureBands();
    void FreeBands();
    // Sends rows [y0, y1) of the frame through the bands, converted to the
    // panel's RGB565 and byte order, with the active overlay blended over
    // them when blendOverlay is set. Reads no other rows of the frame.
    void PushRows(const uint8_t* framebuffer, int width, int height, Deki::ColorFormat format, int y0, int y1,
                  bool blendOverlay);
    // PushRows inside a write the caller holds: what it queues may still be
    // on the wire when it returns.
    void PushRowsBody(const uint8_t* framebuffer, int width, int height, Deki::ColorFormat format, int y0, int y1,
                      bool blendOverlay);
    // Sends rows [y0, y1) of an RGB565 frame already in the panel's byte
    // order straight from the frame, with no copy.
    void PushRowsDirect(const uint16_t* framebuffer, int y0, int y1);
    // PushRowsDirect inside a write the caller holds.
    void PushRowsDirectBody(const uint16_t* frame, int y0, int y1);
    // Copies RGB565 pixels from memory's byte order into the wire's.
    void CopyToWire(uint16_t* dst, const uint16_t* src, size_t pixels) const;
    // Blends the active overlay over rows [y0, y1) of the frame, held in
    // `rows` (RGB565, `pitch` pixels a row, `width` wide, row y0 first), whose
    // pixels are byte-swapped from the CPU's order when `swapped` is set.
    void BlendOverlayRows(uint16_t* rows, size_t pitch, int width, int y0, int y1, bool swapped) const;
    // Asks the engine for a full frame after the overlay changes: the frame's
    // own dirty rectangles know nothing of it.
    void OverlayChanged();
    void Clear();
    bool HasOverlay() const { return m_ActiveOverlay && m_ActiveOverlay->buffer; }

    PanelController& m_Controller;
    IPanelIO& m_IO;
    Options m_Options;
    bool m_Initialized = false;
    int32_t m_Width = 0;
    int32_t m_Height = 0;
    // The wire takes RGB565 high byte first: CPU-order pixels are swapped.
    bool m_WireBigEndian = true;
    // An RGB565 frame (in memory's byte order) is swapped on its way out.
    bool m_SwapRGB565 = false;

    uint16_t* m_Band[2] = { nullptr, nullptr };
    int32_t m_BandRows = 0;
    int m_BandIndex = 0;
    std::vector<Deki::Rect> m_BandScratch;

    uint16_t* m_RenderBuffers[2] = { nullptr, nullptr };
    int m_RenderIndex = 0;

    UIOverlay* m_ActiveOverlay = nullptr;
    int m_Presents = 0;

    // Async present.
    bool m_Async = false;
    bool m_WorkerRunning = false;
    uint8_t* m_Staging[2] = { nullptr, nullptr };
    size_t m_StagingBytes = 0;
    uint16_t* m_Strip[2] = { nullptr, nullptr };
    int m_StripRows = 0;
    // The fewest rows the strip copies could not be grown to: strips that
    // tall go through external memory without asking again (0: none failed).
    int m_StripFailedRows = 0;
    int m_NextCopy = 0;
    // The queue of jobs (at most one per copy, plus the stop) and the copies
    // not in use, under m_Lock.
    std::mutex m_Lock;
    std::condition_variable m_JobReady;
    std::condition_variable m_CopyFree;
    Job m_Jobs[3] = {};
    int m_JobHead = 0;
    int m_JobCount = 0;
    int m_FreeCopies = 2;
    // The worker's last job, whose copy is given back once its parts, up to
    // GetQueuedCount() m_PendingEnd, are sent. Worker only.
    bool m_PendingCopy = false;
    uint32_t m_PendingEnd = 0;
};

}  // namespace DekiDisplay
