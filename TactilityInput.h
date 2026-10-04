#pragma once

#include <cstdint>
#include <vector>

#include "IDekiInput.h"  // from deki-input

struct Device;

namespace DekiTactility
{

/// Deki input on Tactility's raw pointer and keyboard drivers.
///
/// When the game owns the panel, LVGL is not involved: the keyboard driver is
/// the source of key events and LVGL only translates them to LV_KEY_*, so
/// `keyboard_read_key()` gives the full keyboard without LVGL. That is what
/// makes keyboard devices like the T-Deck and the Cardputer usable.
///
/// Both devices are optional: a board with touch and no keyboard, or a
/// keyboard and no touch, works with whichever it has.
///
/// When the game runs in a window (TactilityWindowDisplay), LVGL runs and owns
/// both devices, so input comes from the window's canvas, already in game
/// coordinates.
class TactilityInput : public DekiInput::IDekiInput
{
public:
    TactilityInput() = default;
    ~TactilityInput() override;

    bool Initialize() override;
    void Shutdown() override;
    void Update() override;
    bool IsInitialized() const override { return m_Initialized; }

    void RegisterEventCallback(const DekiInput::InputEventCallback& callback) override;
    bool GetPointerPosition(int32_t* x, int32_t* y) const override;
    bool IsKeyPressed(uint32_t key) const override;

private:
    void PollPointer();
    void PollKeyboard();
    void Emit(const DekiInput::InputEvent& event);
    /// Updates the pointer position and held keys from an event that arrived
    /// ready-made (from a window's canvas).
    void Track(const DekiInput::InputEvent& event);

    /// Tactility reports a Unicode codepoint, never a scan code. Printable
    /// characters pass through as their codepoint; the named keys it spells as
    /// codepoints (arrows at 0x2190.., escape at 0x1B) are translated to the
    /// engine's own ids.
    static uint32_t TranslateCodePoint(uint32_t codepoint);

    /// Limits key reads per frame: the `continue_reading` flag asks for more
    /// reads, and a stuck controller would never stop asking.
    static constexpr int kMaxKeyReadsPerUpdate = 16;

    struct Device* m_Pointer = nullptr;
    struct Device* m_Keyboard = nullptr;
    bool m_Initialized = false;

    std::vector<DekiInput::InputEventCallback> m_Callbacks;

    // Last known pointer state, so down/move/up transitions can be synthesised:
    // Tactility reports the currently touched points, not events.
    bool m_Touched = false;
    int32_t m_TouchX = 0;
    int32_t m_TouchY = 0;

    /// Keys currently held, for IsKeyPressed(). A small linear array: only a
    /// few keys are held at once, and it keeps the heap out of the input path.
    static constexpr int kMaxHeldKeys = 8;
    uint32_t m_HeldKeys[kMaxHeldKeys] = {};
    int m_HeldKeyCount = 0;
};

}  // namespace DekiTactility
