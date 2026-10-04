#pragma once

#include <cstdint>

#include <deki/Engine.h>  // ColorFormat
#include <deki/providers/IDisplay.h>

// Tactility's own types exist only when building an app against its SDK. The
// editor loads this package too (so the SetupComponents can be inspected on a
// desktop), and there the device handle is an opaque pointer that is never
// dereferenced.
struct Device;

namespace DekiTactility
{

/// Deki display on Tactility's raw panel driver.
///
/// LVGL is not used. Deki renders a whole framebuffer, so an lv_canvas would
/// only be an extra surface to blit into, and it would tie the app to the
/// ~450 symbols the lvgl module exports (a missing symbol fails when the app
/// is loaded, not when it is built). The app stops the LVGL module and calls
/// `display_draw_bitmap()` directly, as Tactility's own GraphicsDemo does.
///
/// Why band staging: `display_draw_bitmap()` may DMA straight out of the
/// pointer it is given; DISPLAY_CAPABILITY_PREFER_EXTERNAL_RAM is the panel's
/// promise that it does not. Without that capability the source must be
/// DMA-capable memory, which on ESP32 means internal RAM, and a whole 320x240
/// RGB565 frame (150 KB) does not fit there. So the framebuffer stays where
/// the engine put it (usually PSRAM) and is copied out in `kBandRows`-row
/// bands through a small internal staging buffer, as LovyanGFXDisplay does.
/// A panel with PREFER_EXTERNAL_RAM gets the engine's rows directly.
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

    // Overlays are stubs, as in LovyanGFXDisplay's non-ESP32 branch: no Deki
    // package calls them (only EditorDisplay and the rendering tests do).
    // Implement them when a UI system composites through the display.
    void* CreateUIOverlay(int32_t width, int32_t height) override;
    bool UpdateUIOverlay(void* overlay, int32_t x, int32_t y, int32_t width, int32_t height,
                         const uint32_t* pixels) override;
    bool UpdateUIOverlayRGB565A8(void* overlay, int32_t x, int32_t y, int32_t width, int32_t height,
                                 const uint8_t* rgb565a8Pixels) override;
    void DestroyUIOverlay(void* overlay) override;
    void SetActiveUIOverlay(void* overlay) override;
    void ClearActiveUIOverlay() override;

    void SetBacklight(bool on) override;

    /// The panel's own colour format, queried at Initialize().
    Deki::ColorFormat GetPanelFormat() const { return m_PanelFormat; }

private:
    /// Pushes one half-open rectangle of the framebuffer to the panel.
    void PushRect(const uint8_t* framebuffer, int fbWidth, Deki::ColorFormat format, int32_t x0, int32_t y0, int32_t x1,
                  int32_t y1);

    /// Rows staged per draw_bitmap call. 8, as in LovyanGFXDisplay, keeps the
    /// staging buffer small (320 px * 8 rows * 2 B = 5 KB) while spreading the
    /// driver's per-call cost.
    static constexpr int32_t kBandRows = 8;

    struct Device* m_Device = nullptr;
    bool m_Initialized = false;
    bool m_StoppedLvgl = false;
    /// The panel promises not to DMA from the given pointer, so staging can be
    /// skipped.
    bool m_CanDrawFromExternalRam = false;
    /// The panel wants the other byte order, so every pixel is swapped while
    /// staging.
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
