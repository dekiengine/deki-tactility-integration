#include "TactilityDisplay.h"

#include <cstring>

#include <deki/LogSystem.h>
#include <deki/providers/Memory.h>

#if defined(DEKI_TACTILITY_TARGET)
extern "C"
{
#include <lvgl/module.h>
#include <tactility/device.h>
#include <tactility/drivers/display.h>
#include <tactility/error.h>
#include <tactility/module.h>
}
#endif

// The present task runs on a device only: the simulator's FreeRTOS is a POSIX
// port with no second core to send frames from.
#if defined(DEKI_TACTILITY_TARGET) && defined(ESP_PLATFORM)
#define DEKI_TACTILITY_ASYNC_PRESENT 1
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#else
#define DEKI_TACTILITY_ASYNC_PRESENT 0
#endif

namespace DekiTactility
{

#if defined(DEKI_TACTILITY_TARGET)

namespace
{

/// How long to wait for the panel lock before giving up on a band. On many
/// boards the display shares its bus with the SD card, so the lock can be held
/// for a while; dropping one band is better than blocking the game loop.
constexpr uint32_t kLockTimeoutTicks = 100;

// PROBE (temporary): frame timing.
int64_t g_ProbeStart = 0;
int64_t g_ProbeWaitUs = 0;
int64_t g_ProbeCopyUs = 0;
volatile int64_t g_ProbeSendUs = 0;
uint32_t g_ProbeFrames = 0;

/// Maps the panel's own format to the engine's. Returns false for formats Deki
/// has no equivalent for: MONOCHROME and GRAYSCALE8 (e-paper, some OLEDs)
/// cannot be rendered into yet, so Initialize() refuses them with an error
/// instead of scrambling the panel.
bool MapPanelFormat(DisplayColorFormat panel, Deki::ColorFormat* out, bool* needsByteSwap)
{
    *needsByteSwap = false;
    switch (panel)
    {
        case DISPLAY_COLOR_FORMAT_RGB565: *out = Deki::ColorFormat::RGB565; return true;
        case DISPLAY_COLOR_FORMAT_RGB565_SWAPPED:
            *out = Deki::ColorFormat::RGB565;
            *needsByteSwap = true;
            return true;
        case DISPLAY_COLOR_FORMAT_RGB888: *out = Deki::ColorFormat::RGB888; return true;
        default:
            // BGR565(_SWAPPED) would need a channel swizzle, not just a byte
            // swap; MONOCHROME and GRAYSCALE8 need a different renderer.
            return false;
    }
}

/// Copies `pixels` 16-bit values, swapping each one's bytes.
void CopySwapped16(uint8_t* dst, const uint8_t* src, size_t pixels)
{
    for (size_t i = 0; i < pixels; ++i)
    {
        dst[i * 2 + 0] = src[i * 2 + 1];
        dst[i * 2 + 1] = src[i * 2 + 0];
    }
}

}  // namespace

TactilityDisplay::TactilityDisplay() = default;

TactilityDisplay::~TactilityDisplay()
{
    Shutdown();
}

bool TactilityDisplay::Initialize(int32_t width, int32_t height)
{
    if (m_Initialized)
    {
        return true;
    }

    // Acquire the panel BEFORE stopping LVGL, the order Tactility's own
    // GraphicsDemo uses: the device lookup goes through the running system.
    if (device_get_first_active_by_type(&DISPLAY_TYPE, &m_Device) != ERROR_NONE || m_Device == nullptr)
    {
        DEKI_LOG_ERROR("TactilityDisplay: no active display device");
        return false;
    }

    // Take the panel away from LVGL. Until this returns, LVGL owns the bus and
    // any drawing would race its flushes.
    if (module_is_started(&lvgl_module))
    {
        module_stop(&lvgl_module);
        m_StoppedLvgl = true;
    }

    const DisplayColorFormat panelFormat = display_get_color_format(m_Device);
    bool needsByteSwap = false;
    if (!MapPanelFormat(panelFormat, &m_PanelFormat, &needsByteSwap))
    {
        DEKI_LOG_ERROR("TactilityDisplay: panel colour format %d has no Deki equivalent", (int)panelFormat);
        Shutdown();
        return false;
    }
    m_NeedsByteSwap = needsByteSwap;

    // The panel's own size wins over whatever the caller guessed: the platform
    // bakes assets for the target's resolution, but the OS knows the truth.
    m_DisplayWidth = display_get_resolution_x(m_Device);
    m_DisplayHeight = display_get_resolution_y(m_Device);
    if (m_DisplayWidth <= 0 || m_DisplayHeight <= 0)
    {
        DEKI_LOG_ERROR("TactilityDisplay: panel reported a %dx%d resolution", (int)m_DisplayWidth,
                       (int)m_DisplayHeight);
        Shutdown();
        return false;
    }
    // 0x0 means the caller has no expectation to check against.
    if (width > 0 && height > 0 && (width != m_DisplayWidth || height != m_DisplayHeight))
    {
        DEKI_LOG_WARNING("TactilityDisplay: project expects %dx%d, panel is %dx%d", (int)width, (int)height,
                         (int)m_DisplayWidth, (int)m_DisplayHeight);
    }

    // A panel that promises never to DMA from the caller's pointer gets the
    // engine's rows directly, with no staging copy.
    m_CanDrawFromExternalRam = display_has_capability(m_Device, DISPLAY_CAPABILITY_PREFER_EXTERNAL_RAM);

    if (!m_CanDrawFromExternalRam || m_NeedsByteSwap)
    {
        m_BandBytes = Deki::FrameBufferBytes(m_PanelFormat, m_DisplayWidth, kBandRows);
        // DMA-capable, not merely internal: the peripheral cannot reach some
        // internal regions, and a panel reading one corrupts the display
        // without any error.
        m_Band = (uint8_t*)Deki::Memory::AllocateDma(m_BandBytes, Deki::Memory::Internal);
        if (m_Band == nullptr)
        {
            DEKI_LOG_ERROR("TactilityDisplay: no room for a %zu byte staging band in DMA-capable "
                           "internal RAM",
                           m_BandBytes);
            Shutdown();
            return false;
        }
    }

    // The panel still holds whatever LVGL last drew.
    display_clear(m_Device);

    m_Initialized = true;
    const bool async = StartAsyncPresent();
    DEKI_LOG_INFO("TactilityDisplay: %dx%d, format %d, staging=%s, frames sent %s", (int)m_DisplayWidth,
                  (int)m_DisplayHeight, (int)panelFormat, m_Band ? "yes" : "direct",
                  async ? "from the other core" : "as drawn");
    return true;
}

bool TactilityDisplay::StartAsyncPresent()
{
#if DEKI_TACTILITY_ASYNC_PRESENT && !defined(CONFIG_FREERTOS_UNICORE)
    const size_t bytes = Deki::FrameBufferBytes(m_PanelFormat, m_DisplayWidth, m_DisplayHeight);
    for (QueuedFrame& frame : m_Frames)
    {
        frame.pixels = static_cast<uint8_t*>(Deki::Memory::Allocate(bytes, Deki::Memory::External));
        frame.rects.reserve(16);
    }
    // Two frames, plus one give for the task's goodbye (StopAsyncPresent).
    m_FreeFrames = xSemaphoreCreateCounting(3, 2);
    m_FrameReady = xQueueCreate(2, sizeof(int));
    if (m_Frames[0].pixels == nullptr || m_Frames[1].pixels == nullptr || m_FreeFrames == nullptr ||
        m_FrameReady == nullptr)
    {
        DEKI_LOG_WARNING("TactilityDisplay: no memory for two %u-byte frame copies; frames are sent as they are drawn",
                         (unsigned)bytes);
        StopAsyncPresent();
        return false;
    }

    // The other core from the one the game runs on. Above the game's priority,
    // so a frame starts going out as soon as it is queued.
    const BaseType_t core = xPortGetCoreID() == 0 ? 1 : 0;
    TaskHandle_t task = nullptr;
    if (xTaskCreatePinnedToCore(&PresentTaskMain, "deki_present", 4096, this, uxTaskPriorityGet(nullptr) + 1, &task,
                                core) != pdPASS)
    {
        DEKI_LOG_WARNING("TactilityDisplay: could not start the present task; frames are sent as they are drawn");
        StopAsyncPresent();
        return false;
    }
    m_PresentTask = task;
    m_AsyncPresent = true;
    return true;
#else
    return false;
#endif
}

void TactilityDisplay::PresentTaskMain(void* arg)
{
#if DEKI_TACTILITY_ASYNC_PRESENT
    auto* self = static_cast<TactilityDisplay*>(arg);
    auto* ready = static_cast<QueueHandle_t>(self->m_FrameReady);
    auto* free = static_cast<SemaphoreHandle_t>(self->m_FreeFrames);
    uint32_t frames = 0;
    for (;;)
    {
        int index = 0;
        xQueueReceive(ready, &index, portMAX_DELAY);
        if (index < 0)
        {
            // StopAsyncPresent: say so, then go. Nothing here touches the
            // display again.
            xSemaphoreGive(free);
            vTaskDelete(nullptr);
            return;
        }
        const QueuedFrame& frame = self->m_Frames[index];
        const int64_t sendStart = esp_timer_get_time();
        for (const Deki::Rect& r : frame.rects)
        {
            self->PushRect(frame.pixels, frame.width, self->m_PanelFormat, r.left, r.top, r.right, r.bottom);
        }
        g_ProbeSendUs += esp_timer_get_time() - sendStart;
        xSemaphoreGive(free);

        // At full speed the next frame is always waiting and this task never
        // blocks, so the core's idle task never runs and the task watchdog
        // reports it. A tick's pause every 30 frames lets it.
        if (++frames % 30 == 0)
        {
            vTaskDelay(1);
        }
    }
#else
    (void)arg;
#endif
}

void TactilityDisplay::QueueFrame(const uint8_t* framebuffer, int width, int height, const Deki::Rect* rects,
                                  int32_t count)
{
#if DEKI_TACTILITY_ASYNC_PRESENT
    // Waits only while both copies are queued or being sent.
    const int64_t waitStart = esp_timer_get_time();
    xSemaphoreTake(static_cast<SemaphoreHandle_t>(m_FreeFrames), portMAX_DELAY);
    const int64_t copyStart = esp_timer_get_time();
    g_ProbeWaitUs += copyStart - waitStart;
    const int index = m_NextFrame;
    m_NextFrame = 1 - m_NextFrame;
    QueuedFrame& frame = m_Frames[index];
    frame.width = width;
    frame.height = height;
    frame.rects.clear();

    const size_t stride = Deki::FrameBufferBytes(m_PanelFormat, width, 1);
    if (count < 0)
    {
        frame.rects.push_back({ 0, 0, width, height });
        std::memcpy(frame.pixels, framebuffer, stride * (size_t)height);
    }
    else
    {
        // Only the rows something changed in are copied: the task sends only
        // the rects, so the rest of the copy is never read.
        for (int32_t i = 0; i < count; ++i)
        {
            Deki::Rect r = rects[i];
            r.top = r.top < 0 ? 0 : r.top;
            r.bottom = r.bottom > height ? height : r.bottom;
            if (r.bottom <= r.top || r.right <= r.left)
            {
                continue;
            }
            frame.rects.push_back(r);
            std::memcpy(frame.pixels + (size_t)r.top * stride, framebuffer + (size_t)r.top * stride,
                        stride * (size_t)(r.bottom - r.top));
        }
    }
    g_ProbeCopyUs += esp_timer_get_time() - copyStart;
    xQueueSend(static_cast<QueueHandle_t>(m_FrameReady), &index, portMAX_DELAY);

    // PROBE (temporary): where each frame's time goes, every 2 s.
    if (g_ProbeStart == 0)
    {
        g_ProbeStart = waitStart;
    }
    ++g_ProbeFrames;
    if (waitStart - g_ProbeStart >= 2000000)
    {
        const double n = (double)g_ProbeFrames;
        DEKI_LOG_INFO("PROBE fps=%.1f frame=%.2fms wait=%.2fms copy=%.2fms send=%.2fms rects=%d",
                      n * 1e6 / (double)(waitStart - g_ProbeStart), (double)(waitStart - g_ProbeStart) / n / 1000.0,
                      (double)g_ProbeWaitUs / n / 1000.0, (double)g_ProbeCopyUs / n / 1000.0,
                      (double)g_ProbeSendUs / n / 1000.0, (int)frame.rects.size());
        g_ProbeStart = waitStart;
        g_ProbeFrames = 0;
        g_ProbeWaitUs = g_ProbeCopyUs = 0;
        g_ProbeSendUs = 0;
    }
#else
    (void)framebuffer;
    (void)width;
    (void)height;
    (void)rects;
    (void)count;
#endif
}

void TactilityDisplay::WaitForAsyncPresent()
{
#if DEKI_TACTILITY_ASYNC_PRESENT
    if (!m_AsyncPresent)
    {
        return;
    }
    // Both copies free: nothing queued or being sent.
    auto* free = static_cast<SemaphoreHandle_t>(m_FreeFrames);
    xSemaphoreTake(free, portMAX_DELAY);
    xSemaphoreTake(free, portMAX_DELAY);
    xSemaphoreGive(free);
    xSemaphoreGive(free);
#endif
}

void TactilityDisplay::StopAsyncPresent()
{
#if DEKI_TACTILITY_ASYNC_PRESENT
    if (m_PresentTask != nullptr)
    {
        WaitForAsyncPresent();
        auto* free = static_cast<SemaphoreHandle_t>(m_FreeFrames);
        // Hold both copies, so the only give left is the task's goodbye.
        xSemaphoreTake(free, portMAX_DELAY);
        xSemaphoreTake(free, portMAX_DELAY);
        const int stop = -1;
        xQueueSend(static_cast<QueueHandle_t>(m_FrameReady), &stop, portMAX_DELAY);
        xSemaphoreTake(free, portMAX_DELAY);
        // The give returns before the task has deleted itself; let it finish
        // before its queue and semaphore go.
        vTaskDelay(2);
        m_PresentTask = nullptr;
    }
    if (m_FrameReady != nullptr)
    {
        vQueueDelete(static_cast<QueueHandle_t>(m_FrameReady));
        m_FrameReady = nullptr;
    }
    if (m_FreeFrames != nullptr)
    {
        vSemaphoreDelete(static_cast<SemaphoreHandle_t>(m_FreeFrames));
        m_FreeFrames = nullptr;
    }
    for (QueuedFrame& frame : m_Frames)
    {
        if (frame.pixels != nullptr)
        {
            Deki::Memory::Free(frame.pixels);
            frame.pixels = nullptr;
        }
    }
    m_AsyncPresent = false;
#endif
}

void TactilityDisplay::Shutdown()
{
    // The task uses the band and the device: it stops first.
    StopAsyncPresent();

    if (m_Band != nullptr)
    {
        Deki::Memory::Free(m_Band);
        m_Band = nullptr;
        m_BandBytes = 0;
    }

    // Hand the panel back before releasing it, or the next app inherits a
    // display nothing is driving.
    if (m_StoppedLvgl)
    {
        if (!module_is_started(&lvgl_module))
        {
            module_start(&lvgl_module);
        }
        m_StoppedLvgl = false;
    }

    if (m_Device != nullptr)
    {
        device_put(m_Device);
        m_Device = nullptr;
    }

    m_Initialized = false;
}

void TactilityDisplay::Present(const uint8_t* framebuffer, int width, int height, Deki::ColorFormat format)
{
    if (!m_Initialized || framebuffer == nullptr)
    {
        return;
    }
    if (m_AsyncPresent && format == m_PanelFormat)
    {
        QueueFrame(framebuffer, width, height, nullptr, -1);
        return;
    }
    WaitForAsyncPresent();
    PushRect(framebuffer, width, format, 0, 0, width, height);
}

void TactilityDisplay::PresentRegions(const uint8_t* framebuffer, int width, int height, Deki::ColorFormat format,
                                      const Deki::Rect* rects, int32_t count)
{
    if (!m_Initialized || framebuffer == nullptr)
    {
        return;
    }

    // count == 0 means nothing changed, and an LCD holds its last frame, so
    // there is nothing to do.
    if (count == 0)
    {
        return;
    }

    if (count < 0 || rects == nullptr)
    {
        Present(framebuffer, width, height, format);
        return;
    }

    if (m_AsyncPresent && format == m_PanelFormat)
    {
        QueueFrame(framebuffer, width, height, rects, count);
        return;
    }
    WaitForAsyncPresent();

    // Deki::Rect is {left, top, right, bottom} and half-open, as
    // display_draw_bitmap() wants (its end coordinates are exclusive too), so
    // no conversion is needed.
    for (int32_t i = 0; i < count; ++i)
    {
        PushRect(framebuffer, width, format, rects[i].left, rects[i].top, rects[i].right, rects[i].bottom);
    }
}

void TactilityDisplay::PushRect(const uint8_t* framebuffer, int fbWidth, Deki::ColorFormat format, int32_t x0,
                                int32_t y0, int32_t x1, int32_t y1)
{
    if (format != m_PanelFormat)
    {
        DEKI_LOG_ERROR("TactilityDisplay: frame is format %d but the panel is %d", (int)format, (int)m_PanelFormat);
        return;
    }

    // Never past the panel or the framebuffer: the band holds one panel row
    // per line, and the driver is not asked to draw off its edge. The engine
    // sizes the framebuffer to this panel, so this only ever trims a caller's
    // mistake.
    if (x0 < 0)
    {
        x0 = 0;
    }
    if (y0 < 0)
    {
        y0 = 0;
    }
    if (x1 > m_DisplayWidth)
    {
        x1 = m_DisplayWidth;
    }
    if (y1 > m_DisplayHeight)
    {
        y1 = m_DisplayHeight;
    }
    if (x1 > fbWidth)
    {
        x1 = fbWidth;
    }
    if (x1 <= x0 || y1 <= y0)
    {
        return;
    }

    const size_t bpp = Deki::FrameBufferBytes(format, 1, 1);
    const int32_t rectWidth = x1 - x0;
    const size_t rowBytes = (size_t)rectWidth * bpp;
    const size_t fbStride = (size_t)fbWidth * bpp;

    // A full-width rect out of a buffer the panel may read directly needs no
    // staging at all: its rows are already contiguous. A narrower one from such
    // a buffer has no band to gather into, so each row goes on its own.
    const bool direct = (m_Band == nullptr) && (x0 == 0) && (x1 == fbWidth);
    const bool rowByRow = (m_Band == nullptr) && !direct;

    auto draw = [&](int32_t top, int32_t bottom, const uint8_t* source)
    {
        if (device_try_lock(m_Device, kLockTimeoutTicks))
        {
            // End coordinates are exclusive, matching Tactility's contract.
            const error_t result = display_draw_bitmap(m_Device, x0, top, x1, bottom, source);
            device_unlock(m_Device);
            // A panel that refuses every draw leaves the screen frozen while
            // the game runs unseen, so log it: once with the reason, then as a
            // running count instead of every band.
            if (result != ERROR_NONE)
            {
                ++m_DrawFailures;
                if (m_DrawFailures == 1 || (m_DrawFailures % 1000) == 0)
                {
                    DEKI_LOG_ERROR("TactilityDisplay: the panel refused a draw (%s); %u refused so far",
                                   error_to_string(result), m_DrawFailures);
                }
            }
        }
        else
        {
            DEKI_LOG_WARNING("TactilityDisplay: panel lock timed out, dropped rows %d-%d", (int)top, (int)bottom);
        }
    };

    if (rowByRow)
    {
        for (int32_t y = y0; y < y1; ++y)
        {
            draw(y, y + 1, framebuffer + (size_t)y * fbStride + (size_t)x0 * bpp);
        }
        return;
    }

    for (int32_t y = y0; y < y1; y += kBandRows)
    {
        const int32_t bandEnd = (y + kBandRows < y1) ? (y + kBandRows) : y1;
        const int32_t bandRows = bandEnd - y;

        const uint8_t* source;
        if (direct)
        {
            source = framebuffer + (size_t)y * fbStride;
        }
        else
        {
            // Gather the band's rows into the staging buffer, converting on the
            // way if the panel wants the other byte order.
            for (int32_t r = 0; r < bandRows; ++r)
            {
                const uint8_t* src = framebuffer + (size_t)(y + r) * fbStride + (size_t)x0 * bpp;
                uint8_t* dst = m_Band + (size_t)r * rowBytes;
                if (m_NeedsByteSwap)
                {
                    CopySwapped16(dst, src, (size_t)rectWidth);
                }
                else
                {
                    std::memcpy(dst, src, rowBytes);
                }
            }
            source = m_Band;
        }
        draw(y, bandEnd, source);
    }
}

void TactilityDisplay::GetDisplaySize(int32_t* width, int32_t* height) const
{
    if (width != nullptr)
    {
        *width = m_DisplayWidth;
    }
    if (height != nullptr)
    {
        *height = m_DisplayHeight;
    }
}

void TactilityDisplay::RequestFullRefresh()
{
    if (!m_Initialized)
    {
        return;
    }
    // Meaningful on e-paper, where it clears accumulated ghosting; the driver
    // reports ERROR_NOT_SUPPORTED on panels that have no such operation.
    WaitForAsyncPresent();
    display_refresh(m_Device);
}

bool TactilityDisplay::ProcessEvents()
{
    // Tactility has no display-side event queue: the OS tells an app to quit
    // through APP_EVENT_CLOSE, which the app entry point polls. Returning true
    // here keeps the engine running; the entry point owns the exit decision.
    return true;
}

void TactilityDisplay::SetBacklight(bool on)
{
    (void)on;
    // Tactility exports display_get_backlight but no setter, and the backlight
    // belongs to the OS, so this does nothing.
}

#else  // !DEKI_TACTILITY_TARGET — editor/host build, no Tactility SDK present

TactilityDisplay::TactilityDisplay() = default;
TactilityDisplay::~TactilityDisplay() = default;
bool TactilityDisplay::Initialize(int32_t, int32_t)
{
    return false;
}
void TactilityDisplay::Shutdown()
{
}
void TactilityDisplay::Present(const uint8_t*, int, int, Deki::ColorFormat)
{
}
void TactilityDisplay::PresentRegions(const uint8_t*, int, int, Deki::ColorFormat, const Deki::Rect*, int32_t)
{
}
void TactilityDisplay::GetDisplaySize(int32_t* width, int32_t* height) const
{
    if (width != nullptr)
    {
        *width = 0;
    }
    if (height != nullptr)
    {
        *height = 0;
    }
}
void TactilityDisplay::RequestFullRefresh()
{
}
bool TactilityDisplay::ProcessEvents()
{
    return true;
}
void TactilityDisplay::SetBacklight(bool)
{
}

#endif  // DEKI_TACTILITY_TARGET

// Overlays are stubs on both paths; see the header.
void* TactilityDisplay::CreateUIOverlay(int32_t, int32_t)
{
    return nullptr;
}
bool TactilityDisplay::UpdateUIOverlay(void*, int32_t, int32_t, int32_t, int32_t, const uint32_t*)
{
    return false;
}
bool TactilityDisplay::UpdateUIOverlayRGB565A8(void*, int32_t, int32_t, int32_t, int32_t, const uint8_t*)
{
    return false;
}
void TactilityDisplay::DestroyUIOverlay(void*)
{
}
void TactilityDisplay::SetActiveUIOverlay(void*)
{
}
void TactilityDisplay::ClearActiveUIOverlay()
{
}

}  // namespace DekiTactility
