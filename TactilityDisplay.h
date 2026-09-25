#pragma once

#include <cstdint>

#include <deki/Engine.h>  // ColorFormat
#include <deki/providers/IDisplay.h>

// Tactility's own types are only available when building an app against its
// SDK. The editor loads this package too (so the SetupComponents stay
// inspectable on a desktop), and there the device handle is just an opaque
// pointer we never dereference.
struct Device;

namespace DekiTactility
{

/**
 * @brief Deki display backed by Tactility's raw panel driver.
 *
 * Deliberately does NOT use LVGL. Deki renders a whole framebuffer, so an
 * lv_canvas would only be a surface to blit into — pure overhead, and it would
 * bind us to the curated ~450-symbol subset the lvgl module exports (a symbol
 * missing from that list fails when the app is LOADED, not when it is built).
 *
 * Instead the app stops the LVGL module and drives `display_draw_bitmap()`
 * directly, the way Tactility's own GraphicsDemo does.
 *
 * ## Why band staging
 *
 * `display_draw_bitmap()` MAY DMA straight out of the pointer it is given:
 * Tactility defines DISPLAY_CAPABILITY_PREFER_EXTERNAL_RAM as the panel's
 * promise that it does *not*. So unless a panel advertises that capability the
 * source must be DMA-capable memory, which on ESP32 means internal RAM — and
 * pinning a whole 320x240 RGB565 frame (150 KB) there is not affordable.
 *
 * So the framebuffer stays wherever the engine put it (PSRAM, typically) and we
 * copy it out in `kBandRows`-row bands through a small internal staging buffer.
 * This mirrors what LovyanGFXDisplay already does for the same reason. When the
 * panel does advertise PREFER_EXTERNAL_RAM we skip the staging copy entirely
 * and hand the engine's rows straight over.
 */
class TactilityDisplay : public Deki::IDisplay
{
   public:
    TactilityDisplay();
    ~TactilityDisplay() override;

    bool Initialize(int32_t width, int32_t height) override;
    void Shutdown() override;

    void Present(const uint8_t* framebuffer, int width, int height, Deki::ColorFormat format) override;
    bool SupportsPartialPresent() const override { return true; }
    void PresentRegions(const uint8_t* framebuffer, int width, int height, Deki::ColorFormat format,
                        const Deki::Rect* rects, int32_t count) override;

    void GetDisplaySize(int32_t* width, int32_t* height) const override;
    Deki::ColorFormat GetColorFormat() const override { return m_PanelFormat; }
    bool IsInitialized() const override { return m_Initialized; }
    void RequestFullRefresh() override;
    bool ProcessEvents() override;

    // Overlay support: not implemented. Nothing in any Deki package calls these
    // today (only EditorDisplay and the rendering tests do), and the non-ESP32
    // branch of LovyanGFXDisplay stubs them the same way. Implement them here
    // the day a UI system actually composites through the display.
    void* CreateUIOverlay(int32_t width, int32_t height) override;
    bool UpdateUIOverlay(void* overlay, int32_t x, int32_t y, int32_t width, int32_t height,
                         const uint32_t* pixels) override;
    bool UpdateUIOverlayRGB565A8(void* overlay, int32_t x, int32_t y, int32_t width, int32_t height,
                                 const uint8_t* rgb565a8_pixels) override;
    void DestroyUIOverlay(void* overlay) override;
    void SetActiveUIOverlay(void* overlay) override;
    void ClearActiveUIOverlay() override;

    void SetBacklight(bool on) override;

    /// The panel's own colour format, queried at Initialize().
    Deki::ColorFormat GetPanelFormat() const { return m_PanelFormat; }

   private:
    /// Push one half-open rectangle of the framebuffer to the panel.
    void PushRect(const uint8_t* framebuffer, int fbWidth, Deki::ColorFormat format, int32_t x0, int32_t y0,
                  int32_t x1, int32_t y1);

    /// Rows staged per draw_bitmap call. 8 matches LovyanGFXDisplay; it keeps
    /// the staging buffer small (320 px * 8 rows * 2 B = 5 KB) while still
    /// amortising the per-call overhead of the driver.
    static constexpr int32_t kBandRows = 8;

    struct Device* m_Device = nullptr;
    bool m_Initialized = false;
    bool m_StoppedLvgl = false;
    /// Panel promises not to DMA from our pointer, so staging can be skipped.
    bool m_CanDrawFromExternalRam = false;
    /// Panel wants the other byte order, so every pixel is swapped while staging.
    bool m_NeedsByteSwap = false;

    int32_t m_DisplayWidth = 0;
    int32_t m_DisplayHeight = 0;
    Deki::ColorFormat m_PanelFormat = Deki::ColorFormat::RGB565;

    /// DMA-capable staging band, allocated at Initialize().
    uint8_t* m_Band = nullptr;
    size_t m_BandBytes = 0;

    /// Draws the panel has refused, for the log.
    uint32_t m_DrawFailures = 0;
};

}  // namespace DekiTactility
