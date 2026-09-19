#include "TactilityInput.h"
#include "TactilityKeys.h"
#include "TactilityWindowDisplay.h"

#include <deki/LogSystem.h>
#include <deki/Time.h>

#if defined(DEKI_TACTILITY_TARGET)
extern "C"
{
#include <tactility/device.h>
#include <tactility/drivers/keyboard.h>
#include <tactility/drivers/pointer.h>
}
#endif

namespace DekiTactility
{

uint32_t TactilityInput::TranslateCodePoint(uint32_t codepoint)
{
#if defined(DEKI_TACTILITY_TARGET)
    switch (codepoint)
    {
        // Enter, escape and backspace already agree: Tactility spells them as
        // the C0 control codes, which is exactly what Deki's ids are.
        case CODEPOINT_ENTER:     return Keys::kEnter;
        case CODEPOINT_ESCAPE:    return Keys::kEsc;
        case CODEPOINT_BACKSPACE: return Keys::kBackspace;
        case CODEPOINT_ARROW_UP:    return Keys::kUp;
        case CODEPOINT_ARROW_DOWN:  return Keys::kDown;
        case CODEPOINT_ARROW_LEFT:  return Keys::kLeft;
        case CODEPOINT_ARROW_RIGHT: return Keys::kRight;
        default:
            break;
    }
#endif

    // Printable ASCII passes straight through, the same range and convention
    // deki-sdl3-integration uses. Anything else (HOME, END, non-Latin
    // codepoints) has no Deki id and is reported as 0 = unmapped.
    if (codepoint >= 32 && codepoint <= 126)
        return codepoint;
    return 0;
}

TactilityInput::~TactilityInput()
{
    Shutdown();
}

void TactilityInput::RegisterEventCallback(const DekiInput::InputEventCallback& callback)
{
    m_Callbacks.push_back(callback);
}

void TactilityInput::Emit(const DekiInput::InputEvent& event)
{
    for (const auto& callback : m_Callbacks)
    {
        if (callback)
            callback(event);
    }
}

bool TactilityInput::GetPointerPosition(int32_t* x, int32_t* y) const
{
    if (x != nullptr)
        *x = m_TouchX;
    if (y != nullptr)
        *y = m_TouchY;
    return m_Initialized;
}

bool TactilityInput::IsKeyPressed(uint32_t key) const
{
    for (int i = 0; i < m_HeldKeyCount; ++i)
    {
        if (m_HeldKeys[i] == key)
            return true;
    }
    return false;
}

#if defined(DEKI_TACTILITY_TARGET)

bool TactilityInput::Initialize()
{
    if (m_Initialized)
        return true;

    // Both devices are optional. A board may have touch and no keyboard (most
    // panels), or a keyboard and no touch (Cardputer); only having neither is
    // a failure, since then this backend can produce nothing.
    if (device_get_first_active_by_type(&POINTER_TYPE, &m_Pointer) != ERROR_NONE)
        m_Pointer = nullptr;
    if (device_get_first_active_by_type(&KEYBOARD_TYPE, &m_Keyboard) != ERROR_NONE)
        m_Keyboard = nullptr;

    if (m_Pointer == nullptr && m_Keyboard == nullptr)
    {
        DEKI_LOG_ERROR("TactilityInput: no pointer and no keyboard device");
        return false;
    }

    m_Initialized = true;
    DEKI_LOG_INFO("TactilityInput: pointer=%s keyboard=%s",
                  m_Pointer ? "yes" : "no", m_Keyboard ? "yes" : "no");
    return true;
}

void TactilityInput::Shutdown()
{
    if (m_Pointer != nullptr)
    {
        device_put(m_Pointer);
        m_Pointer = nullptr;
    }
    if (m_Keyboard != nullptr)
    {
        device_put(m_Keyboard);
        m_Keyboard = nullptr;
    }
    m_Callbacks.clear();
    m_HeldKeyCount = 0;
    m_Touched = false;
    m_Initialized = false;
}

void TactilityInput::Update()
{
    if (!m_Initialized)
        return;

    // In a window, LVGL is running and owns the pointer and keyboard: reading
    // the devices here as well would split the keystrokes between the two.
    // The window's canvas collects them instead.
    if (TactilityWindowDisplay* window = TactilityWindowDisplay::Active())
    {
        window->DrainInput([this](const DekiInput::InputEvent& event) { Track(event); Emit(event); });
        return;
    }

    PollPointer();
    PollKeyboard();
}

void TactilityInput::Track(const DekiInput::InputEvent& event)
{
    switch (event.type)
    {
        case DekiInput::InputEventType::MOUSE_BUTTON_DOWN:
        case DekiInput::InputEventType::MOUSE_MOVE:
            m_Touched = (event.type == DekiInput::InputEventType::MOUSE_BUTTON_DOWN) || m_Touched;
            m_TouchX = event.x;
            m_TouchY = event.y;
            break;
        case DekiInput::InputEventType::MOUSE_BUTTON_UP:
            m_Touched = false;
            break;
        case DekiInput::InputEventType::KEY_DOWN:
            if (!IsKeyPressed(event.key) && m_HeldKeyCount < kMaxHeldKeys)
                m_HeldKeys[m_HeldKeyCount++] = event.key;
            break;
        case DekiInput::InputEventType::KEY_UP:
            for (int i = 0; i < m_HeldKeyCount; ++i)
            {
                if (m_HeldKeys[i] == event.key)
                {
                    m_HeldKeys[i] = m_HeldKeys[--m_HeldKeyCount];
                    break;
                }
            }
            break;
        default:
            break;
    }
}

void TactilityInput::PollPointer()
{
    if (m_Pointer == nullptr)
        return;

    // read_data() refreshes the controller's cached state; get_touched_points()
    // then reports it. Tactility gives us the CURRENT points, not events, so
    // the down/move/up transitions are ours to synthesise.
    pointer_read_data(m_Pointer, 0);

    uint16_t xs[1] = {};
    uint16_t ys[1] = {};
    uint16_t strengths[1] = {};
    uint8_t count = 0;
    pointer_get_touched_points(m_Pointer, xs, ys, strengths, &count, 1);

    const bool touched = (count > 0);
    const uint32_t now = Deki::Time::GetTime();

    DekiInput::InputEvent event = {};
    event.timestamp = now;

    if (touched)
    {
        const int32_t x = (int32_t)xs[0];
        const int32_t y = (int32_t)ys[0];

        if (!m_Touched)
        {
            m_Touched = true;
            m_TouchX = x;
            m_TouchY = y;
            event.type = DekiInput::InputEventType::MOUSE_BUTTON_DOWN;
            event.x = x;
            event.y = y;
            event.pressed = true;
            Emit(event);
        }
        else if (x != m_TouchX || y != m_TouchY)
        {
            m_TouchX = x;
            m_TouchY = y;
            event.type = DekiInput::InputEventType::MOUSE_MOVE;
            event.x = x;
            event.y = y;
            Emit(event);
        }
    }
    else if (m_Touched)
    {
        m_Touched = false;
        event.type = DekiInput::InputEventType::MOUSE_BUTTON_UP;
        event.x = m_TouchX;
        event.y = m_TouchY;
        event.pressed = false;
        Emit(event);
    }
}

void TactilityInput::PollKeyboard()
{
    if (m_Keyboard == nullptr)
        return;

    for (int reads = 0; reads < kMaxKeyReadsPerUpdate; ++reads)
    {
        struct KeyboardKeyData data = {};
        if (keyboard_read_key(m_Keyboard, &data) != ERROR_NONE)
            break;

        const uint32_t key = TranslateCodePoint(data.key);
        if (key != 0)
        {
            DekiInput::InputEvent event = {};
            event.type = data.pressed ? DekiInput::InputEventType::KEY_DOWN
                                      : DekiInput::InputEventType::KEY_UP;
            event.key = key;
            event.pressed = data.pressed;
            event.timestamp = Deki::Time::GetTime();
            Emit(event);

            if (data.pressed)
            {
                if (!IsKeyPressed(key) && m_HeldKeyCount < kMaxHeldKeys)
                    m_HeldKeys[m_HeldKeyCount++] = key;
            }
            else
            {
                for (int i = 0; i < m_HeldKeyCount; ++i)
                {
                    if (m_HeldKeys[i] == key)
                    {
                        m_HeldKeys[i] = m_HeldKeys[--m_HeldKeyCount];
                        break;
                    }
                }
            }
        }

        // The driver sets this when more key data is already queued. Without
        // draining, a fast typist's keystrokes arrive one per frame.
        if (!data.continue_reading)
            break;
    }
}

#else  // !DEKI_TACTILITY_TARGET — editor/host build, no Tactility SDK present

bool TactilityInput::Initialize() { return false; }
void TactilityInput::Shutdown() {}
void TactilityInput::Update() {}
void TactilityInput::Track(const DekiInput::InputEvent&) {}
void TactilityInput::PollPointer() {}
void TactilityInput::PollKeyboard() {}

#endif  // DEKI_TACTILITY_TARGET

}  // namespace DekiTactility
