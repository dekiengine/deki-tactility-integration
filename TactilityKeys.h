#pragma once

#include <cstdint>

namespace DekiTactility::Keys
{

// Deki's generic key ids. The engine keeps them in src/InputKeys.h, which is
// private, so each input package repeats the few it needs (as
// deki-sdl3-integration does). The raw keyboard (TactilityInput) and LVGL's
// keys (TactilityWindowDisplay) both map onto this one copy.
inline constexpr uint32_t kEnter = 13;
inline constexpr uint32_t kEsc = 27;
inline constexpr uint32_t kBackspace = 8;
inline constexpr uint32_t kUp = 1001;
inline constexpr uint32_t kDown = 1002;
inline constexpr uint32_t kLeft = 1003;
inline constexpr uint32_t kRight = 1004;

}  // namespace DekiTactility::Keys
