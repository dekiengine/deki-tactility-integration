#include "TactilityWindowDisplay.h"
#include "TactilityKeys.h"

#include <cstring>

#include <deki/LogSystem.h>
#include <deki/Time.h>
#include <deki/providers/Memory.h>

#if defined(DEKI_TACTILITY_TARGET)
extern "C"
{
#include <app/scheduler.h>
#include <lvgl.h>
#include <lvgl/lvgl.h>
#include <lvgl_window_manager/window_manager.h>
}
#endif

namespace DekiTactility
{

namespace
{
TactilityWindowDisplay* s_ActiveWindow = nullptr;
}  // namespace

TactilityWindowDisplay* TactilityWindowDisplay::Active()
{
    return s_ActiveWindow;
}

TactilityWindowDisplay::TactilityWindowDisplay() = default;

TactilityWindowDisplay::~TactilityWindowDisplay()
{
    Shutdown();
}

void TactilityWindowDisplay::GetDisplaySize(int32_t* width, int32_t* height) const
{
    if (width != nullptr)
        *width = m_Width;
    if (height != nullptr)
        *height = m_Height;
}

#if defined(DEKI_TACTILITY_TARGET)

namespace
{
void CreateWidgetsThunk(lv_obj_t* root, void* self)
{
    static_cast<TactilityWindowDisplay*>(self)->BuildWidgets(root);
}

void DestroyWidgetsThunk(void* self)
{
    static_cast<TactilityWindowDisplay*>(self)->DropWidgets();
}

void CanvasEventThunk(lv_event_t* event)
{
    static_cast<TactilityWindowDisplay*>(lv_event_get_user_data(event))->HandleCanvasEvent(event);
}

/// LVGL's key codes to Deki's. Printable ASCII passes through; anything else
/// is 0 = unmapped.
uint32_t TranslateLvglKey(uint32_t key)
{
    switch (key)
    {
        case LV_KEY_ENTER:     return Keys::kEnter;
        case LV_KEY_ESC:       return Keys::kEsc;
        case LV_KEY_BACKSPACE: return Keys::kBackspace;
        case LV_KEY_UP:        return Keys::kUp;
        case LV_KEY_DOWN:      return Keys::kDown;
        case LV_KEY_LEFT:      return Keys::kLeft;
        case LV_KEY_RIGHT:     return Keys::kRight;
        default:
            break;
    }
    if (key >= 32 && key <= 126)
        return key;
    return 0;
}
}  // namespace

bool TactilityWindowDisplay::Lock()
{
    lvgl_lock();
    return true;
}

void TactilityWindowDisplay::Unlock()
{
    lvgl_unlock();
}

bool TactilityWindowDisplay::Initialize(int32_t, int32_t)
{
    if (m_Initialized)
        return true;

    if (!lvgl_is_running())
    {
        DEKI_LOG_ERROR("TactilityWindowDisplay: LVGL is not running, so there is no window to show the game in");
        return false;
    }

    // The game's own size: the engine renders at the platform's screen size,
    // and the window shows that 1:1, centred.
    m_Width = DEKI_SCREEN_WIDTH;
    m_Height = DEKI_SCREEN_HEIGHT;
    m_Buffer = Deki::Memory::AllocateArray<uint8_t>((size_t)m_Width * (size_t)m_Height * 2, Deki::Memory::External);
    if (m_Buffer == nullptr)
    {
        DEKI_LOG_ERROR("TactilityWindowDisplay: no memory for a %dx%d canvas", (int)m_Width, (int)m_Height);
        return false;
    }
    std::memset(m_Buffer, 0, (size_t)m_Width * (size_t)m_Height * 2);

    s_ActiveWindow = this;
    // Builds the canvas right away (the new window is on top), on LVGL's
    // side of the lock.
    m_WindowId = window_manager_create_ext(app_scheduler_current_app_id(), CreateWidgetsThunk, DestroyWidgetsThunk,
                                           this);
    if (m_WindowId == 0)
    {
        DEKI_LOG_ERROR("TactilityWindowDisplay: the window manager is not running");
        Shutdown();
        return false;
    }

    m_Initialized = true;
    DEKI_LOG_INFO("TactilityWindowDisplay: %dx%d in an app window", (int)m_Width, (int)m_Height);
    return true;
}

void TactilityWindowDisplay::Shutdown()
{
    if (m_WindowId != 0)
    {
        // Deletes the canvas before the buffer it shows goes. A removed
        // window's record goes with it, so DropWidgets is not called: forget
        // the canvas here.
        window_manager_remove(m_WindowId);
        m_WindowId = 0;
        Lock();
        m_Canvas = nullptr;
        m_PointerDown = false;
        Unlock();
    }
    if (m_Buffer != nullptr)
    {
        Deki::Memory::Free(m_Buffer);
        m_Buffer = nullptr;
    }
    if (s_ActiveWindow == this)
        s_ActiveWindow = nullptr;
    m_Initialized = false;
}

void TactilityWindowDisplay::BuildWidgets(void* rootObject)
{
    auto* root = static_cast<lv_obj_t*>(rootObject);
    lv_obj_t* canvas = lv_canvas_create(root);
    lv_canvas_set_buffer(canvas, m_Buffer, m_Width, m_Height, LV_COLOR_FORMAT_RGB565);
    lv_obj_center(canvas);

    // Pointer and keys reach the game through the canvas.
    lv_obj_add_flag(canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(canvas, CanvasEventThunk, LV_EVENT_ALL, this);
    if (lv_group_t* group = lv_group_get_default())
    {
        lv_group_add_obj(group, canvas);
        lv_group_focus_obj(canvas);
    }

    lv_obj_update_layout(root);
    lv_area_t area;
    lv_obj_get_coords(canvas, &area);
    m_CanvasX = area.x1;
    m_CanvasY = area.y1;
    m_Canvas = canvas;
}

void TactilityWindowDisplay::DropWidgets()
{
    // The window manager deletes the widgets itself; this only forgets them,
    // so a present arriving meanwhile sees there is nowhere to draw.
    m_Canvas = nullptr;
    m_PointerDown = false;
}

void TactilityWindowDisplay::Queue(const DekiInput::InputEvent& event)
{
    if (m_QueueCount < kQueueSize)
        m_Queue[m_QueueCount++] = event;
}

void TactilityWindowDisplay::HandleCanvasEvent(void* eventObject)
{
    auto* event = static_cast<lv_event_t*>(eventObject);
    const lv_event_code_t code = lv_event_get_code(event);

    DekiInput::InputEvent out = {};
    out.timestamp = Deki::Time::GetTime();

    if (code == LV_EVENT_PRESSED || code == LV_EVENT_PRESSING || code == LV_EVENT_RELEASED ||
        code == LV_EVENT_PRESS_LOST)
    {
        lv_indev_t* indev = lv_indev_active();
        if (indev == nullptr)
            return;
        lv_point_t point;
        lv_indev_get_point(indev, &point);
        out.x = point.x - m_CanvasX;
        out.y = point.y - m_CanvasY;

        if (code == LV_EVENT_PRESSED)
        {
            m_PointerDown = true;
            out.type = DekiInput::InputEventType::MOUSE_BUTTON_DOWN;
            out.pressed = true;
        }
        else if (code == LV_EVENT_PRESSING)
        {
            out.type = DekiInput::InputEventType::MOUSE_MOVE;
        }
        else
        {
            if (!m_PointerDown)
                return;
            m_PointerDown = false;
            out.type = DekiInput::InputEventType::MOUSE_BUTTON_UP;
        }
        Queue(out);
    }
    else if (code == LV_EVENT_KEY)
    {
        const uint32_t key = TranslateLvglKey(lv_event_get_key(event));
        if (key == 0)
            return;
        // LVGL reports a key once, as it goes down (again on repeat), and
        // never as it comes up.
        out.key = key;
        out.type = DekiInput::InputEventType::KEY_DOWN;
        out.pressed = true;
        Queue(out);
        out.type = DekiInput::InputEventType::KEY_UP;
        out.pressed = false;
        Queue(out);
    }
}

void TactilityWindowDisplay::CopyRect(const uint8_t* framebuffer, int fbWidth, int32_t x0, int32_t y0, int32_t x1,
                                      int32_t y1)
{
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > m_Width) x1 = m_Width;
    if (y1 > m_Height) y1 = m_Height;
    if (x1 <= x0 || y1 <= y0)
        return;
    const size_t rowBytes = (size_t)(x1 - x0) * 2;
    for (int32_t y = y0; y < y1; ++y)
        std::memcpy(m_Buffer + ((size_t)y * m_Width + x0) * 2, framebuffer + ((size_t)y * fbWidth + x0) * 2,
                    rowBytes);
}

void TactilityWindowDisplay::PresentRegions(const uint8_t* framebuffer, int width, int height,
                                            Deki::ColorFormat format, const Deki::Rect* rects, int32_t count)
{
    if (!m_Initialized || framebuffer == nullptr || count == 0)
        return;
    if (format != Deki::ColorFormat::RGB565)
    {
        static bool s_Warned = false;
        if (!s_Warned)
            DEKI_LOG_ERROR("TactilityWindowDisplay: frames are format %d; the window shows RGB565 only", (int)format);
        s_Warned = true;
        return;
    }

    Lock();
    if (m_Canvas != nullptr)
    {
        if (count < 0 || rects == nullptr)
        {
            CopyRect(framebuffer, width, 0, 0, width, height);
        }
        else
        {
            for (int32_t i = 0; i < count; ++i)
                CopyRect(framebuffer, width, rects[i].left, rects[i].top, rects[i].right, rects[i].bottom);
        }
        // Handing the buffer back drops LVGL's cached copy of the image and
        // redraws it: a canvas written to directly otherwise shows its old
        // pixels.
        lv_canvas_set_buffer(static_cast<lv_obj_t*>(m_Canvas), m_Buffer, m_Width, m_Height,
                             LV_COLOR_FORMAT_RGB565);
    }
    Unlock();
}

void TactilityWindowDisplay::Present(const uint8_t* framebuffer, int width, int height, Deki::ColorFormat format)
{
    PresentRegions(framebuffer, width, height, format, nullptr, -1);
}

#else  // !DEKI_TACTILITY_TARGET — editor/host build, no Tactility SDK present

bool TactilityWindowDisplay::Lock() { return false; }
void TactilityWindowDisplay::Unlock() {}
bool TactilityWindowDisplay::Initialize(int32_t, int32_t) { return false; }
void TactilityWindowDisplay::Shutdown() {}
void TactilityWindowDisplay::BuildWidgets(void*) {}
void TactilityWindowDisplay::DropWidgets() {}
void TactilityWindowDisplay::HandleCanvasEvent(void*) {}
void TactilityWindowDisplay::Queue(const DekiInput::InputEvent&) {}
void TactilityWindowDisplay::CopyRect(const uint8_t*, int, int32_t, int32_t, int32_t, int32_t) {}
void TactilityWindowDisplay::PresentRegions(const uint8_t*, int, int, Deki::ColorFormat, const Deki::Rect*, int32_t) {}
void TactilityWindowDisplay::Present(const uint8_t*, int, int, Deki::ColorFormat) {}

#endif  // DEKI_TACTILITY_TARGET

}  // namespace DekiTactility
