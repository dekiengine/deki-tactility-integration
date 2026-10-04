#pragma once

#include <deki/SetupComponent.h>
#include <deki/reflection/Property.h>

#include "TactilityPackage.h"

namespace DekiTactility
{

/// Boot component that feeds Tactility's touch and keys into Deki. Reads the
/// pointer and keyboard drivers directly, without LVGL, and uses whichever of
/// the two the board has.
DEKI_CATEGORY("Tactility")
DEKI_DESCRIPTION("Feeds Tactility's touch and keyboard into Deki's input system.")
class DEKI_TACTILITY_API TactilityInputSetup : public Deki::SetupComponent
{
public:
    void Setup(SetupCallback onComplete) override;
    const char* GetSetupName() const override { return "Tactility Input"; }
};

}  // namespace DekiTactility
