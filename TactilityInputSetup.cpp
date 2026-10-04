#include "TactilityInputSetup.h"

#include <deki/LogSystem.h>

#if !defined(DEKI_EDITOR) && defined(DEKI_PACKAGE_TACTILITY)
#include <memory>

#include "DekiInput.h"      // from deki-input
#include "DekiInputInit.h"  // DekiInputInitSystem (from deki-input)
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

    // deki-input's class has the same name as its namespace, so a bare
    // DekiInput:: names the namespace, which has no SetInput. The alias names
    // the class, as in deki-sdl3-integration.
    using DekiInputApi = DekiInput::DekiInput;
    DekiInputApi::SetInput(std::move(input), "Tactility");

    // Creates and registers the dispatch system; safe to call twice. Global
    // scope, not DekiTactility::, because the editor's generated glue
    // declares it there.
    DekiInputInitSystem();

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
