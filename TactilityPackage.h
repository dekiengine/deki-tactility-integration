#pragma once

/**
 * @file TactilityPackage.h
 * @brief Central header for the Deki Tactility Package
 *
 * Runs a Deki game as an external Tactility app, without LVGL:
 * - Display setup (takes the panel over from Tactility's LVGL module)
 * - Input setup (raw pointer and keyboard drivers)
 * - Filesystem (F:/ and S:/ mapped onto the app's assets and user data)
 */

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
