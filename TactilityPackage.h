#pragma once

// Main header of the Deki Tactility package, which runs a Deki game as an
// external Tactility app:
// - Display setup (takes the panel over from Tactility's LVGL module, or runs
//   in a window)
// - Input setup (raw pointer and keyboard drivers)
// - Filesystem (F:/ and S:/ mapped onto the app's assets and user data)

// DLL export macro
#ifdef _WIN32
#ifdef DEKI_TACTILITY_EXPORTS
#define DEKI_TACTILITY_API __declspec(dllexport)
#else
#define DEKI_TACTILITY_API __declspec(dllimport)
#endif
#else
#define DEKI_TACTILITY_API __attribute__((visibility("default")))
#endif
