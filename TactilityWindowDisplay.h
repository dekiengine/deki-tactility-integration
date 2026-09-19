#pragma once

#include <cstdint>

#include <deki/Engine.h>  // ColorFormat
#include <deki/providers/IDisplay.h>

#include "IDekiInput.h"  // from deki-input

namespace DekiTactility
{

/**
 * @brief Deki display shown in an app window, with LVGL and the OS left running.
 *
 * The other way to run: TactilityDisplay takes the whole panel over and stops
 * LVGL, so the game owns the screen and nothing else draws. This one asks
 * Tactility's window manager for a window, like any LVGL app, and shows the
 * game's framebuffer in a canvas inside it: the status bar, the launcher and
 * the rest of the OS stay up around it.
 *
 * The game renders at its own size (the platform's screen size), placed where
 * it would be on the bare panel - centred on the screen - with the OS's chrome
 * drawn over it: a game the size of the screen fills it, and the status bar
 * hides its top rows. Each present copies the finished frame into the canvas's buffer
 * under the LVGL lock, and LVGL draws it from its own task. That is also what
 * makes it visible in the simulator, whose renderer belongs to LVGL's thread
 * and shows nothing drawn from any other.
 *
 * Input: LVGL owns the pointer and keyboard while it runs, so the canvas
 * collects their events (on LVGL's task) and TactilityInput drains them on the
 * game's. LVGL reports a key when it is pressed, not when it is released, so
 * a key arrives as a press immediately followed by a release.
 */
class TactilityWindowDisplay : public Deki::IDisplay
{
   public:
    TactilityWindowDisplay();
    ~TactilityWindowDisplay() override;

    bool Initialize(int32_t width, int32_t height) override;
    void Shutdown() override;

    void Present(const uint8_t* framebuffer, int width, int height, Deki::ColorFormat format) override;
    bool SupportsPartialPresent() const override { return true; }
    void PresentRegions(const uint8_t* framebuffer, int width, int height, Deki::ColorFormat format,
                        const Deki::Rect* rects, int32_t count) override;

    void GetDisplaySize(int32_t* width, int32_t* height) const override;
    bool IsInitialized() const override { return m_Initialized; }
    void RequestFullRefresh() override {}
    bool ProcessEvents() override { return true; }

    void* CreateUIOverlay(int32_t, int32_t) override { return nullptr; }
    bool UpdateUIOverlay(void*, int32_t, int32_t, int32_t, int32_t, const uint32_t*) override { return false; }
    bool UpdateUIOverlayRGB565A8(void*, int32_t, int32_t, int32_t, int32_t, const uint8_t*) override { return false; }
    void DestroyUIOverlay(void*) override {}
    void SetActiveUIOverlay(void*) override {}
    void ClearActiveUIOverlay() override {}
    void SetBacklight(bool) override {}

    /// The window display in use, if the game runs in a window; null when it
    /// owns the panel. TactilityInput asks, to know where its input comes from.
    static TactilityWindowDisplay* Active();

    /// Hand every input event the canvas has collected since the last call to
    /// `emit`, in order. Called on the game's task.
    template <typename Emit>
    void DrainInput(Emit&& emit);

    // Called from the window manager's and LVGL's callbacks, on LVGL's task
    // with the LVGL lock held; not for game code. The parameters are LVGL
    // objects, typed void* so this header needs no LVGL.
    void BuildWidgets(void* root);
    void DropWidgets();
    void HandleCanvasEvent(void* event);

   private:
    bool Lock();
    void Unlock();
    void Queue(const DekiInput::InputEvent& event);
    /// Copy one half-open rectangle into the canvas's buffer (LVGL lock held).
    void CopyRect(const uint8_t* framebuffer, int fbWidth, int32_t x0, int32_t y0, int32_t x1, int32_t y1);

    bool m_Initialized = false;
    int32_t m_Width = 0;
    int32_t m_Height = 0;

    /// The canvas's pixels, RGB565, owned here and lent to LVGL.
    uint8_t* m_Buffer = nullptr;

    uint32_t m_WindowId = 0;
    /// The canvas, while the window is on top. Only touched under the LVGL
    /// lock; null while another window covers this one.
    void* m_Canvas = nullptr;
    /// The canvas's top-left on screen, for turning pointer positions into
    /// game coordinates.
    int32_t m_CanvasX = 0;
    int32_t m_CanvasY = 0;
    bool m_PointerDown = false;

    /// Events from LVGL's task, waiting for the game's. Guarded by the LVGL
    /// lock, which both sides already hold when they touch it. Fixed size: a
    /// frame's worth of input is a handful of events, and a full queue drops
    /// the newest rather than allocating on LVGL's task.
    static constexpr int kQueueSize = 64;
    DekiInput::InputEvent m_Queue[kQueueSize] = {};
    int m_QueueCount = 0;
};

template <typename Emit>
void TactilityWindowDisplay::DrainInput(Emit&& emit)
{
    if (!Lock())
        return;
    DekiInput::InputEvent pending[kQueueSize];
    const int count = m_QueueCount;
    for (int i = 0; i < count; ++i)
        pending[i] = m_Queue[i];
    m_QueueCount = 0;
    Unlock();
    // Outside the lock: a game callback may do anything, including block.
    for (int i = 0; i < count; ++i)
        emit(pending[i]);
}

}  // namespace DekiTactility
