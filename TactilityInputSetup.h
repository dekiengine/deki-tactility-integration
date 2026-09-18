#pragma once

#include <deki/SetupComponent.h>
#include <deki/reflection/Property.h>

#include "TactilityPackage.h"

namespace DekiTactility
{

/**
 * @brief Boot component that feeds Tactility's touch and keys into Deki.
 *
 * Reads the pointer and keyboard drivers directly — no LVGL, and no LVGL input
 * devices. Whichever of the two the board actually has is used.
 */
DEKI_CATEGORY("Tactility")
DEKI_DESCRIPTION("Feeds Tactility's touch and keyboard into Deki's input system.")
class DEKI_TACTILITY_API TactilityInputSetup : public Deki::SetupComponent
{
   public:
    void Setup(SetupCallback onComplete) override;
    const char* GetSetupName() const override { return "Tactility Input"; }
};

}  // namespace DekiTactility
