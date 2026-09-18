#include "TactilityInputSetup.h"

#include <deki/LogSystem.h>

#if !defined(DEKI_EDITOR) && defined(DEKI_PACKAGE_TACTILITY)
#include <memory>

#include "DekiInput.h"      // from deki-input
#include "DekiInputInit.h"  // DekiInput_InitSystem (from deki-input)
#include "TactilityInput.h"
#endif

namespace DekiTactility
{

#if !defined(DEKI_EDITOR) && defined(DEKI_PACKAGE_TACTILITY)

void TactilityInputSetup::Setup(SetupCallback onComplete)
{
    auto input = std::make_unique<TactilityInput>();
    if (!input->Initialize())
    {
        DEKI_LOG_ERROR("TactilityInputSetup: failed to open the input devices");
        onComplete(false);
        return;
    }

    // deki-input's class shares its name with its namespace, so a bare
    // DekiInput:: resolves to the namespace, where SetInput is not a member.
    // The alias says which is meant once — same as deki-sdl3-integration.
    using DekiInputApi = DekiInput::DekiInput;
    DekiInputApi::SetInput(std::move(input), "Tactility");

    // Create and register the dispatch system. Global scope, not
    // DekiTactility::, because the editor's generated glue declares it that
    // way. Idempotent.
    DekiInput_InitSystem();

    DEKI_LOG_INFO("TactilityInputSetup: input ready");
    onComplete(true);
}

#else

void TactilityInputSetup::Setup(SetupCallback onComplete)
{
    onComplete(true);
}

#endif

}  // namespace DekiTactility
