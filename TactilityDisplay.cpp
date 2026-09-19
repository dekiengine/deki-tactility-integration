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

namespace DekiTactility
{

#if defined(DEKI_TACTILITY_TARGET)

namespace
{

/// How long to wait for the panel lock before giving up on a band.
/// The display shares its bus with the SD card on many boards, so a lock can
/// legitimately be held for a while; dropping one band is better than blocking
/// the game loop indefinitely.
constexpr uint32_t kLockTimeoutTicks = 100;

/// Map the panel's own format onto the engine's.
///
/// Returns false for formats Deki has no equivalent for. MONOCHROME and
/// GRAYSCALE8 are real Tactility formats (e-paper, some OLEDs) that the engine
/// cannot currently render into, so they are refused loudly at Initialize()
/// rather than producing a scrambled panel at runtime.
bool MapPanelFormat(DisplayColorFormat panel, Deki::ColorFormat* out, bool* needsByteSwap)
{
    *needsByteSwap = false;
    switch (panel)
    {
        case DISPLAY_COLOR_FORMAT_RGB565:
            *out = Deki::ColorFormat::RGB565;
            return true;
        case DISPLAY_COLOR_FORMAT_RGB565_SWAPPED:
            *out = Deki::ColorFormat::RGB565;
            *needsByteSwap = true;
            return true;
        case DISPLAY_COLOR_FORMAT_RGB888:
            *out = Deki::ColorFormat::RGB888;
            return true;
        default:
            // BGR565(_SWAPPED) would need a channel swizzle, not just a byte
            // swap; MONOCHROME and GRAYSCALE8 need a different renderer.
            return false;
    }
}

/// Copy `pixels` 16-bit values, swapping each one's bytes.
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
        return true;

    // Acquire the panel BEFORE stopping LVGL, the order Tactility's own
    // GraphicsDemo uses: the device lookup goes through the running system.
    if (device_get_first_active_by_type(&DISPLAY_TYPE, &m_Device) != ERROR_NONE || m_Device == nullptr)
    {
        DEKI_LOG_ERROR("TactilityDisplay: no active display device");
        return false;
    }

    // Take the panel away from LVGL. Until this returns, LVGL owns the bus and
    // anything we draw would race its flushes.
    if (module_is_started(&lvgl_module))
    {
        module_stop(&lvgl_module);
        m_StoppedLvgl = true;
    }

    const DisplayColorFormat panelFormat = display_get_color_format(m_Device);
    bool needsByteSwap = false;
    if (!MapPanelFormat(panelFormat, &m_PanelFormat, &needsByteSwap))
    {
        DEKI_LOG_ERROR("TactilityDisplay: panel colour format %d has no Deki equivalent",
                       (int)panelFormat);
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
        DEKI_LOG_ERROR("TactilityDisplay: panel reported a %dx%d resolution",
                       (int)m_DisplayWidth, (int)m_DisplayHeight);
        Shutdown();
        return false;
    }
    // 0x0 means the caller has no expectation to check against.
    if (width > 0 && height > 0 && (width != m_DisplayWidth || height != m_DisplayHeight))
    {
        DEKI_LOG_WARNING("TactilityDisplay: project expects %dx%d, panel is %dx%d",
                         (int)width, (int)height, (int)m_DisplayWidth, (int)m_DisplayHeight);
    }

    // A panel that promises never to DMA from the caller's pointer lets us skip
    // the staging copy and hand the engine's own rows straight to the driver.
    m_CanDrawFromExternalRam =
        display_has_capability(m_Device, DISPLAY_CAPABILITY_PREFER_EXTERNAL_RAM);

    if (!m_CanDrawFromExternalRam || m_NeedsByteSwap)
    {
        m_BandBytes = Deki::FrameBufferBytes(m_PanelFormat, m_DisplayWidth, kBandRows);
        // DMA-capable, not merely internal: some internal regions are not
        // reachable by the peripheral, and a panel reading one of those
        // corrupts the display silently rather than failing.
        m_Band = (uint8_t*)Deki::Memory::AllocateDma(m_BandBytes, Deki::Memory::Internal);
        if (m_Band == nullptr)
        {
            DEKI_LOG_ERROR("TactilityDisplay: no room for a %zu byte staging band in DMA-capable "
                           "internal RAM", m_BandBytes);
            Shutdown();
            return false;
        }
    }

    // The panel still holds whatever LVGL last drew.
    display_clear(m_Device);

    m_Initialized = true;
    DEKI_LOG_INFO("TactilityDisplay: %dx%d, format %d, staging=%s",
                  (int)m_DisplayWidth, (int)m_DisplayHeight, (int)panelFormat,
                  m_Band ? "yes" : "direct");
    return true;
}

void TactilityDisplay::Shutdown()
{
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
            module_start(&lvgl_module);
        m_StoppedLvgl = false;
    }

    if (m_Device != nullptr)
    {
        device_put(m_Device);
        m_Device = nullptr;
    }

    m_Initialized = false;
}

void TactilityDisplay::Present(const uint8_t* framebuffer, int width, int height,
                               Deki::ColorFormat format)
{
    if (!m_Initialized || framebuffer == nullptr)
        return;
    PushRect(framebuffer, width, format, 0, 0, width, height);
}

void TactilityDisplay::PresentRegions(const uint8_t* framebuffer, int width, int height,
                                      Deki::ColorFormat format, const Deki::Rect* rects,
                                      int32_t count)
{
    if (!m_Initialized || framebuffer == nullptr)
        return;

    // count == 0 means nothing changed; an LCD holds its last frame, so there
    // is genuinely nothing to do.
    if (count == 0)
        return;

    if (count < 0 || rects == nullptr)
    {
        Present(framebuffer, width, height, format);
        return;
    }

    // Deki::Rect is {left, top, right, bottom} and half-open, which is exactly
    // what display_draw_bitmap() wants — its end coordinates are exclusive too.
    // No conversion, and no off-by-one to get wrong.
    for (int32_t i = 0; i < count; ++i)
        PushRect(framebuffer, width, format, rects[i].left, rects[i].top, rects[i].right,
                 rects[i].bottom);
}

void TactilityDisplay::PushRect(const uint8_t* framebuffer, int fbWidth, Deki::ColorFormat format,
                                int32_t x0, int32_t y0, int32_t x1, int32_t y1)
{
    if (format != m_PanelFormat)
    {
        DEKI_LOG_ERROR("TactilityDisplay: frame is format %d but the panel is %d",
                       (int)format, (int)m_PanelFormat);
        return;
    }

    if (x1 <= x0 || y1 <= y0)
        return;

    const size_t bpp = Deki::FrameBufferBytes(format, 1, 1);
    const int32_t rectWidth = x1 - x0;
    const size_t rowBytes = (size_t)rectWidth * bpp;
    const size_t fbStride = (size_t)fbWidth * bpp;

    // A full-width rect out of a buffer the panel may read directly needs no
    // staging at all: its rows are already contiguous.
    const bool direct = (m_Band == nullptr) && (x0 == 0) && (x1 == fbWidth);

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
                    CopySwapped16(dst, src, (size_t)rectWidth);
                else
                    std::memcpy(dst, src, rowBytes);
            }
            source = m_Band;
        }

        if (device_try_lock(m_Device, kLockTimeoutTicks))
        {
            // End coordinates are exclusive, matching Tactility's contract.
            const error_t result = display_draw_bitmap(m_Device, x0, y, x1, bandEnd, source);
            device_unlock(m_Device);
            // A panel that refuses every draw leaves the screen on whatever it
            // showed last while the game runs on unseen, so say so - once with
            // the reason, then as a running count rather than every band.
            if (result != ERROR_NONE)
            {
                ++m_DrawFailures;
                if (m_DrawFailures == 1 || (m_DrawFailures % 1000) == 0)
                    DEKI_LOG_ERROR("TactilityDisplay: the panel refused a draw (%s); %u refused so far",
                                   error_to_string(result), m_DrawFailures);
            }
        }
        else
        {
            DEKI_LOG_WARNING("TactilityDisplay: panel lock timed out, dropped rows %d-%d",
                             (int)y, (int)bandEnd);
        }
    }
}

void TactilityDisplay::GetDisplaySize(int32_t* width, int32_t* height) const
{
    if (width != nullptr)
        *width = m_DisplayWidth;
    if (height != nullptr)
        *height = m_DisplayHeight;
}

void TactilityDisplay::RequestFullRefresh()
{
    if (!m_Initialized)
        return;
    // Meaningful on e-paper, where it clears accumulated ghosting; the driver
    // reports ERROR_NOT_SUPPORTED on panels that have no such operation.
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
    // Tactility exports display_get_backlight but no setter, and backlight is
    // the OS's business anyway. Left deliberately unimplemented.
}

#else  // !DEKI_TACTILITY_TARGET — editor/host build, no Tactility SDK present

TactilityDisplay::TactilityDisplay() = default;
TactilityDisplay::~TactilityDisplay() = default;
bool TactilityDisplay::Initialize(int32_t, int32_t) { return false; }
void TactilityDisplay::Shutdown() {}
void TactilityDisplay::Present(const uint8_t*, int, int, Deki::ColorFormat) {}
void TactilityDisplay::PresentRegions(const uint8_t*, int, int, Deki::ColorFormat,
                                      const Deki::Rect*, int32_t) {}
void TactilityDisplay::GetDisplaySize(int32_t* width, int32_t* height) const
{
    if (width != nullptr)
        *width = 0;
    if (height != nullptr)
        *height = 0;
}
void TactilityDisplay::RequestFullRefresh() {}
bool TactilityDisplay::ProcessEvents() { return true; }
void TactilityDisplay::SetBacklight(bool) {}

#endif  // DEKI_TACTILITY_TARGET

// Overlay support is unimplemented on both paths — see the header.
void* TactilityDisplay::CreateUIOverlay(int32_t, int32_t) { return nullptr; }
bool TactilityDisplay::UpdateUIOverlay(void*, int32_t, int32_t, int32_t, int32_t, const uint32_t*)
{
    return false;
}
bool TactilityDisplay::UpdateUIOverlayRGB565A8(void*, int32_t, int32_t, int32_t, int32_t,
                                               const uint8_t*)
{
    return false;
}
void TactilityDisplay::DestroyUIOverlay(void*) {}
void TactilityDisplay::SetActiveUIOverlay(void*) {}
void TactilityDisplay::ClearActiveUIOverlay() {}

}  // namespace DekiTactility
