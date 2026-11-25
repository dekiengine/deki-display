#pragma once

#include "DekiDisplayPackage.h"
#include "MipiDsi.h"

#include "ScanOrderedDisplay.h"  // from deki-rendering

#include <deki/Engine.h>
#include <deki/providers/IDisplay.h>

#include <cstdint>
#include <vector>

namespace DekiDisplay
{

/// A MIPI-DSI panel in video mode, as the engine's display.
///
/// The panel is streamed whole frames from the platform's frame buffers, so
/// there is nothing to send: the game draws straight into a buffer the panel
/// is not reading (GetRenderBuffer), and Present shows it from the next
/// refresh. A frame is never drawn over while it is shown. With two buffers
/// the game waits for the refresh still sending the other one to end; with
/// three there is always one free, and it does not wait.
///
/// It takes only whole frames (SupportsPartialPresent is false): a frame is
/// drawn into a buffer that is shown whole. Redraw Changes still draws only
/// what changed, into each buffer what it is owed.
///
/// With Options::strips it takes frames in strips too (IScanOrderedDisplay):
/// the renderer draws each strip in one of two buffers in internal RAM, much
/// faster than the PSRAM the frame buffers are in, and each finished strip is
/// copied into the frame being put together while the next is drawn (by the
/// platform's DMA where it has one, StartRowsCopy). Present then shows that
/// frame; rows no strip brought (nothing changed there) come from the frame
/// shown before it.
class DEKI_DISPLAY_API DsiDisplay : public Deki::IDisplay, public DekiRendering::IScanOrderedDisplay
{
public:
    struct Options
    {
        /// The backlight's enable pin, -1 for none.
        int32_t backlightPin = -1;
        /// The pin is low for on.
        bool backlightInvert = false;
        /// Drive the backlight pin with PWM at this frequency, 0 to switch
        /// it on and off. A backlight driver made for PWM can flicker with
        /// its enable held high.
        uint32_t backlightPwmHz = 0;
        /// The PWM backlight's brightness when on, 0 to 1.
        float brightness = 1.0f;
        /// How long Present and GetRenderBuffer wait for a refresh to end
        /// before giving up and drawing anyway (logged once).
        uint32_t refreshTimeoutMs = 100;
        /// Take frames in strips drawn in internal RAM (see the class).
        bool strips = false;
    };

    /// `panel` has been sent its start-up and is streaming (Start); it must
    /// outlive this display.
    DsiDisplay(IDsiPanel& panel, int32_t width, int32_t height, uint32_t refreshPeriodUs);
    ~DsiDisplay() override;

    /// Clears every frame buffer to black, shows buffer 0 and turns the
    /// backlight on. False when the panel has no frame buffer.
    bool Start(const Options& options);

    // ---- IDisplay ----
    bool Initialize(int32_t width, int32_t height) override;
    void Shutdown() override;
    /// Shows `framebuffer`: one of the panel's buffers (the one lent to the
    /// game) is shown as it is; any other is copied into the free buffer
    /// first.
    void Present(const uint8_t* framebuffer, int width, int height, Deki::ColorFormat format) override;
    void GetDisplaySize(int32_t* width, int32_t* height) const override;
    Deki::ColorFormat GetColorFormat() const override { return Deki::ColorFormat::RGB565; }
    bool IsInitialized() const override { return m_Started; }
    void RequestFullRefresh() override {}
    bool ProcessEvents() override { return true; }
    /// A buffer the panel is not reading (waiting for one with two buffers),
    /// the same one until the next Present.
    uint8_t* GetRenderBuffer(int32_t* width, int32_t* height) override;
    void SetBacklight(bool on) override;
    /// The backlight's brightness, 0 to 1, from now on; with a PWM backlight
    /// only (on and off otherwise). Kept across SetBacklight(false/true).
    void SetBrightness(float brightness);
    /// Whether the backlight is dimmed with PWM.
    bool HasBrightness() const { return m_Pwm; }
    /// With two buffers each frame waits for a refresh, so frames are timed
    /// to it; with one, frames are written while shown, and it is 0.
    uint32_t GetRefreshPeriodUs() const override;
    bool GetFramesShown(uint64_t* frames, uint64_t* lastShownUs) const override;

    // Overlays are not used: nothing in the engine creates one.
    void* CreateUIOverlay(int32_t, int32_t) override { return nullptr; }
    bool UpdateUIOverlay(void*, int32_t, int32_t, int32_t, int32_t, const uint32_t*) override { return false; }
    bool UpdateUIOverlayRGB565A8(void*, int32_t, int32_t, int32_t, int32_t, const uint8_t*) override { return false; }
    void DestroyUIOverlay(void*) override {}
    void SetActiveUIOverlay(void*) override {}
    void ClearActiveUIOverlay() override {}

    // ---- IScanOrderedDisplay (Options::strips) ----
    DekiRendering::ScanOrder GetScanOrder() const override { return DekiRendering::ScanOrder::RowsDown; }
    /// Copies the strip into the frame being put together: by the platform
    /// in the background from a buffer this display lent, at once from any
    /// other. The first strip of a frame picks the frame buffer it goes into.
    void PresentStrip(const DekiRendering::ScanStrip& strip) override;
    bool LendsStripBuffers() const override { return m_StripsOn; }
    /// One of two buffers in internal RAM, in turn; null when there is no
    /// memory for them (the renderer then draws in its own).
    uint8_t* GetStripBuffer(size_t bytes) override;
    // Part of a strip goes into the frame buffer as it is: only that is drawn,
    // and the rest comes from the frame shown, as far as it is not there yet.
    bool SendsPartOfAStrip() const override { return true; }
    /// Shows the frame the strips were put together into: the last strip's
    /// copy finished, and the rows none brought from the frame shown before.
    void EndStrips() override;

    /// The buffer on screen (for tests and screen captures).
    int32_t GetShownIndex() const { return m_Shown; }

private:
    // A buffer the panel is not reading, waiting for refreshes to end while
    // there is none; after three, the next one anyway, said once in the log.
    int32_t FreeBuffer();
    void SayLate();
    // Copies whole rows [y, y + rows) of `src` (rows `stride` pixels apart)
    // into frame buffer `index`, by the platform or on the CPU, and waits.
    void CopyRowsNow(int32_t index, int32_t y, int32_t rows, const uint8_t* src, int32_t stride);
    // Rows [y, y + rows) of the frame shown last, into frame buffer `index`.
    void KeepShownRows(int32_t index, int32_t y, int32_t rows);
    void FreeStripBuffers();

    IDsiPanel& m_Panel;
    int32_t m_Width = 0;
    int32_t m_Height = 0;
    uint32_t m_RefreshUs = 0;
    Options m_Options;
    bool m_Started = false;
    bool m_Pwm = false;        // the backlight is the platform's PWM
    bool m_LightOn = false;
    int32_t m_Shown = 0;  // the buffer shown last
    int32_t m_Lent = -1;  // the buffer lent to the game until the next Present, -1 for none
    bool m_SaidCopying = false;
    bool m_SaidLate = false;
    bool m_SaidFormat = false;

    // Strips (Options::strips).
    bool m_StripsOn = false;
    uint8_t* m_StripBuffers[2] = { nullptr, nullptr };
    size_t m_StripBytes = 0;
    size_t m_StripsFailed = 0;  // the smallest size the two could not be had at, 0 for none
    int32_t m_NextStrip = 0;
    int32_t m_Assembling = -1;    // the frame buffer this frame's strips go into, -1 for none yet
    // Columns [left, right) of a row; empty for none.
    struct Span
    {
        int32_t left = 0;
        int32_t right = 0;
        bool Empty() const { return right <= left; }
    };
    // Of each row, the columns strips brought this frame: from the first part
    // a row got to the last, the stale columns between them brought from the
    // frame shown.
    std::vector<Span> m_Drawn;
    // For each frame buffer, the columns of each row it holds that may differ
    // from the frame shown last. A frame drawn in part copies from the one
    // shown only these, less what it drew itself. (With three buffers a
    // buffer is one or two frames behind: that is what those frames redrew,
    // and while one place animates it draws them again itself.)
    static constexpr int32_t kMaxBuffers = 4;
    std::vector<Span> m_Stale[kMaxBuffers];
    void MarkShown(int32_t index, const Span* drawn);
    // Copies into buffer `index` from the frame shown what it holds stale and
    // this frame did not draw: whole rows by the platform's DMA, pieces of
    // rows on the CPU (a DMA copy drops the CPU's cache over the rows it
    // writes, so it never takes a row the CPU wrote this frame).
    void KeepStaleParts(int32_t index);
    int32_t Slot(int32_t index) const { return index < kMaxBuffers ? index : kMaxBuffers - 1; }
};

}  // namespace DekiDisplay
