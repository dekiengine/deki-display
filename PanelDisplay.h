#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

#include <deki/Engine.h>  // ColorFormat
#include <deki/providers/IDisplay.h>

#include "IPanelIO.h"
#include "PanelController.h"
#include "PanelTurn.h"
#include "ScanOrderedDisplay.h"  // from deki-rendering

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
///
/// With the panel's tearing-effect line wired (Options::tePin) frames never
/// tear. A panel refreshes its own rows one after another, at its own rate;
/// a frame that is written while the refresh passes shows half old and half
/// new. So each frame starts when the TE line says a refresh starts, and goes
/// out in the panel's own row order, which a write outruns a refresh in. A
/// panel shown turned (landscape on a portrait panel) has its rows across
/// the screen's columns: the controller is then kept at its own orientation
/// and the screen is turned here as it is sent, and the renderer is asked
/// (IScanOrderedDisplay) for strips of columns, in the order the panel
/// refreshes them.
class PanelDisplay : public Deki::IDisplay, public DekiRendering::IScanOrderedDisplay
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
        /// The panel's tearing-effect output, -1 for none. With it, frames
        /// are sent in step with the panel's refresh, in its own row order.
        int32_t tePin = -1;
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

    // DekiRendering::IScanOrderedDisplay, offered while frames wait for the
    // tearing-effect line.
    DekiRendering::ScanOrder GetScanOrder() const override { return m_ScanOrder; }
    void PresentStrip(const DekiRendering::ScanStrip& strip) override;
    // Just under the strips whose copies there was no internal memory for.
    int32_t GetMaxStripPixels() const override;
    // Sent turned, a strip goes out of an internal strip copy the worker only
    // reads: the copy is lent to be drawn in, and sent with no copying.
    bool LendsStripBuffers() const override;
    uint8_t* GetStripBuffer(size_t bytes) override;

    /// Frames start on the panel's refresh (a TE pin is set and pulsing).
    bool IsTearFree() const { return m_TePin >= 0 && !m_TeLost; }

    /// How far into a refresh of `refreshUs` (microseconds since it started)
    /// a frame that missed its start can begin without tearing, when frames
    /// take `writeUs` to go out (0: not known yet). From there to the next
    /// start, the write cannot catch the refresh up before it ends, and is
    /// ahead of the next. `refreshUs` or more: wait for the next start. 0: a
    /// frame that slow tears wherever it starts, so it starts at once.
    static uint32_t TearFreeStartUs(uint32_t refreshUs, uint32_t writeUs);
    /// The screen is turned here as it is sent: the controller stays at its
    /// own orientation.
    bool IsTurned() const { return m_Turned; }

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
        /// Microseconds, monotonic: how long frames take to go out, so a
        /// tear-free frame that missed a refresh's start need not wait for
        /// the next one. Null: frames wait for the next refresh.
        uint64_t (*micros)() = nullptr;
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
    // m_Strip[index]; or the order to stop. Rows are the panel's (the
    // screen's unless turned). frameStart: wait for the panel's refresh
    // first. wire: the strip is in the wire's byte order already. raw: the
    // copy holds screen pixels as drawn, the pixel at screen (srcLeft,
    // srcTop) first and srcStride to a row, which the worker turns into the
    // panel's rows as it sends them.
    struct Job
    {
        int index;
        int top;
        int bottom;
        bool strip;
        bool stop;
        bool frameStart = false;
        bool wire = false;
        bool raw = false;
        int srcLeft = 0;
        int srcTop = 0;
        int srcStride = 0;
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
    // Gives back the copy TakeCopy last gave, unused: it is the next again.
    void GiveBackCopy(int index);
    int m_Lent = -1;  // the strip copy lent to be drawn in, until its PresentStrip
    bool CanPresentAsync(const uint8_t* framebuffer, int width, int height, Deki::ColorFormat format) const;
    // Copies rows [top, bottom) of the frame for the worker and queues them.
    // False when this present cannot go that way.
    bool PresentRowsAsync(const uint8_t* framebuffer, int width, int height, Deki::ColorFormat format, int top,
                          int bottom, bool frameStart);
    bool EnsureStrips(int rows);
    void FreeStrips();
    // Queues a strip copy, swapped in place a chunk at a time unless it is
    // in the wire's order already, inside the worker's held write.
    void SendStripBody(uint16_t* strip, int top, int rows, bool swap);

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
    // PresentRegions, with whether the regions start a frame (each call
    // does, but strips after a frame's first do not).
    void PresentRegionsFrom(const uint8_t* framebuffer, int width, int height, Deki::ColorFormat format,
                            const Deki::Rect* rects, int32_t count, bool frameStart);

    // ---- Tear-free (Options::tePin) ----
    // Turns TE on, and the controller to its own orientation when the screen
    // is turned. In Start, before the panel is cleared.
    void StartTearFree(int32_t tePin);
    // Waits until a frame can start without tearing: the next refresh's
    // start, or, for a frame that missed one, the point in this refresh from
    // which its write cannot catch the refresh up (see the .cpp). A TE line
    // that does not pulse is said once and then not waited for.
    void WaitForRefresh();
    // Waits for the panel's next refresh to start.
    void WaitForRefreshStart();
    // Notes that a part of the frame has gone out (been queued), for how long
    // a frame takes to go out.
    void NotePartSent();
    // The worker backend's clock, or 0 without one.
    static uint64_t Micros();
    // The panel rows [*first, *last) that hold the screen's `rect`.
    void PanelRowsOf(const Deki::Rect& rect, int* first, int* last) const;
    // Fills panel rows [first, last), whole, from screen pixels: the pixel at
    // screen (left, top) is src[0] and screen rows are `stride` pixels apart;
    // the panel rows need only pixels of that area. In the wire's byte order,
    // with the overlay blended over them.
    void GatherPanelRows(uint16_t* dst, const uint16_t* src, int32_t stride, int32_t left, int32_t top, int first,
                         int last) const;
    // Sends panel rows [first, last) through the bands, gathered from screen
    // pixels as GatherPanelRows has them, inside a write the caller holds.
    void PushPanelRowsBody(const uint16_t* src, int32_t stride, int32_t left, int32_t top, int first, int last);
    // A turned frame or strip, sent as drawn or queued for the worker: panel
    // rows [first, last) of the screen pixels `src` holds, a `srcWidth` x
    // `srcHeight` area with its top-left pixel at screen (left, top).
    // `lent`: src is strip copy m_Strip[lent], lent and drawn in, so it is
    // not copied.
    void PresentTurned(const uint16_t* src, int32_t stride, int32_t left, int32_t top, int32_t srcWidth,
                       int32_t srcHeight, int first, int last, bool frameStart, bool stripCopy, int lent = -1);
    // The worker reads the overlay while it turns what it sends: changes to
    // it wait for the worker first.
    void BeforeOverlayChange();

    PanelController& m_Controller;
    IPanelIO& m_IO;
    Options m_Options;
    bool m_Initialized = false;
    int32_t m_Width = 0;
    int32_t m_Height = 0;
    // The controller's own size: the screen's, or, turned, the panel's.
    int32_t m_PanelWidth = 0;
    int32_t m_PanelHeight = 0;
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

    // Tear-free.
    int32_t m_TePin = -1;
    std::atomic<bool> m_TeLost{ false };  // set by whichever core waited
    // Measuring the refresh rate, on whichever core waits.
    uint32_t m_RefreshSince = 0;
    bool m_RefreshSaid = false;
    // Once measured, how long a refresh takes, and how long the last two
    // frames took to go out, from their first part to their last.
    uint32_t m_RefreshUs = 0;
    uint64_t m_FrameStartUs = 0;
    uint64_t m_LastPartUs = 0;
    uint32_t m_WriteUs[2] = { 0, 0 };
    bool m_Turned = false;
    PixelMap m_ToScreen;  // a panel pixel's screen pixel, turned
    PixelMap m_ToPanel;   // and back
    DekiRendering::ScanOrder m_ScanOrder = DekiRendering::ScanOrder::RowsDown;
    bool m_TurnWarned = false;

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
