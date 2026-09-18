#pragma once

#include <cstdint>
#include <vector>

#include "IDekiInput.h"  // from deki-input

struct Device;

namespace DekiTactility
{

/**
 * @brief Deki input backed by Tactility's raw pointer and keyboard drivers.
 *
 * No LVGL. Tactility's keyboard driver is the *source* of key events and LVGL
 * is only a translator of them into LV_KEY_* — its own header says so — so an
 * app that never starts LVGL still gets a full keyboard through
 * `keyboard_read_key()`. That is what makes keyboard devices like the T-Deck
 * and the Cardputer usable here.
 *
 * Both devices are optional: a board with a touch panel and no keyboard, or a
 * keyboard and no touch, works with whichever it has.
 */
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

    /// Tactility reports a Unicode codepoint, never a scan code. Printable
    /// characters pass through as their codepoint; the named keys it spells as
    /// codepoints (arrows at 0x2190.., escape at 0x1B) are translated to the
    /// engine's own ids.
    static uint32_t TranslateCodePoint(uint32_t codepoint);

    /// Guards against a wedged keyboard driver monopolising a frame: the
    /// `continue_reading` flag asks us to drain, but a stuck controller would
    /// otherwise never stop asking.
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

    /// Keys currently held, for IsKeyPressed(). Small and linear on purpose:
    /// a handful of keys are held at once and this avoids a heap container in
    /// the input path.
    static constexpr int kMaxHeldKeys = 8;
    uint32_t m_HeldKeys[kMaxHeldKeys] = {};
    int m_HeldKeyCount = 0;
};

}  // namespace DekiTactility
