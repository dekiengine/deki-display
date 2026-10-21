#pragma once

#include <cstddef>
#include <cstdint>
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

private:
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
    // Sends rows [y0, y1) of an RGB565 frame already in the panel's byte
    // order straight from the frame, with no copy.
    void PushRowsDirect(const uint16_t* framebuffer, int y0, int y1);
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
};

}  // namespace DekiDisplay
