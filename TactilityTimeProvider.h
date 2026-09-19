#pragma once

#include <deki/providers/ITimeProvider.h>

#include <cstdint>

// Tactility's own clock: FreeRTOS ticks, 1 kHz on every Tactility target
// (the simulator's FreeRTOSConfig.h insists it matches the ESP32). Delaying
// through it yields the task, so the OS keeps running while a frame waits.
#if defined(DEKI_TACTILITY_TARGET)
#include <tactility/delay.h>
#include <tactility/time.h>
#endif

namespace DekiTactility
{

class TactilityTimeProvider : public Deki::ITimeProvider
{
public:
#if defined(DEKI_TACTILITY_TARGET)
    uint32_t GetTicksMs() const override { return static_cast<uint32_t>(get_millis()); }
    void DelayMs(uint32_t ms) const override { delay_millis(ms); }
#else
    // Inert off-target, so the editor can still compile the package.
    uint32_t GetTicksMs() const override { return 0; }
    void DelayMs(uint32_t) const override {}
#endif
};

}  // namespace DekiTactility
