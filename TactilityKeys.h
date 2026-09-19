#pragma once

#include <cstdint>

namespace DekiTactility::Keys
{

// Deki's generic key ids. The engine keeps these in src/InputKeys.h, which is
// private to the engine, so every input package restates the handful it needs —
// deki-sdl3-integration does the same. One copy for this package: the raw
// keyboard (TactilityInput) and LVGL's keys (TactilityWindowDisplay) both map
// onto them.
inline constexpr uint32_t kEnter = 13;
inline constexpr uint32_t kEsc = 27;
inline constexpr uint32_t kBackspace = 8;
inline constexpr uint32_t kUp = 1001;
inline constexpr uint32_t kDown = 1002;
inline constexpr uint32_t kLeft = 1003;
inline constexpr uint32_t kRight = 1004;

}  // namespace DekiTactility::Keys
