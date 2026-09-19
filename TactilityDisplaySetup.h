#pragma once

#include <cstdint>

#include <deki/SetupComponent.h>
#include <deki/reflection/Property.h>

#include "TactilityPackage.h"

namespace DekiTactility
{

/// How the game shares the screen with Tactility.
enum class TactilityDisplayMode : uint8_t
{
    /// Take the whole panel over: LVGL stops and the game owns every pixel.
    Panel = 0,
    /// Run in an app window: LVGL and the OS (status bar, launcher) stay up
    /// and the game is shown at its own size inside the window.
    Window = 1,
};

/**
 * @brief Boot component that gives the game its display on Tactility.
 *
 * Add this to a Tactility platform's boot scene. In Panel mode the panel's
 * resolution is a fact the OS reports, so it is queried rather than
 * configured; in Window mode the game keeps the platform's screen size.
 */
DEKI_CATEGORY("Tactility")
DEKI_DESCRIPTION("Gives the game Tactility's display: the whole panel, or a window with the OS still running.")
class DEKI_TACTILITY_API TactilityDisplaySetup : public Deki::SetupComponent
{
   public:
    DEKI_EXPORT
    DEKI_TOOLTIP("Panel takes the whole display over and stops LVGL, so the game owns every pixel. Window runs the game in an app window with LVGL and the OS still running around it, at the platform's screen size.")
    TactilityDisplayMode mode = TactilityDisplayMode::Panel;

    void Setup(SetupCallback onComplete) override;
    const char* GetSetupName() const override { return "Tactility Display"; }
};

}  // namespace DekiTactility
