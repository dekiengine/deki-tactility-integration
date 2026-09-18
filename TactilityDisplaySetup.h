#pragma once

#include <cstdint>

#include <deki/SetupComponent.h>
#include <deki/reflection/Property.h>

#include "TactilityPackage.h"

namespace DekiTactility
{

/**
 * @brief Boot component that takes the panel over from Tactility's LVGL.
 *
 * Add this to a Tactility platform's boot scene. It has no size properties on
 * purpose: unlike a desktop window, the panel's resolution is a fact the OS
 * reports, so it is queried rather than configured.
 */
DEKI_CATEGORY("Tactility")
DEKI_DESCRIPTION("Takes the display over from Tactility's LVGL and renders the game to it.")
class DEKI_TACTILITY_API TactilityDisplaySetup : public Deki::SetupComponent
{
   public:
    void Setup(SetupCallback onComplete) override;
    const char* GetSetupName() const override { return "Tactility Display"; }
};

}  // namespace DekiTactility
