#include "TactilityDisplaySetup.h"

#include <deki/LogSystem.h>

#if !defined(DEKI_EDITOR) && defined(DEKI_PACKAGE_TACTILITY)
#include <memory>

#include <deki/Engine.h>

#include "TactilityDisplay.h"
#endif

namespace DekiTactility
{

#if !defined(DEKI_EDITOR) && defined(DEKI_PACKAGE_TACTILITY)

// The package owns the display's lifetime; engine-core only holds the pointer.
static std::unique_ptr<TactilityDisplay> s_Display;

void TactilityDisplaySetup::Setup(SetupCallback onComplete)
{
    s_Display = std::make_unique<TactilityDisplay>();

    // 0x0 means "no expectation": on Tactility the OS owns the panel and
    // reports its real resolution, so there is nothing useful to assert here.
    // (The project's baked target size lives in dproject.bin, which the engine
    // reads during boot — not reliably before this component runs.)
    if (s_Display && s_Display->Initialize(0, 0))
    {
        Deki::Engine::GetInstance().SetDisplay(s_Display.get(), "Tactility");
        DEKI_LOG_INFO("TactilityDisplaySetup: display ready");
        onComplete(true);
    }
    else
    {
        DEKI_LOG_ERROR("TactilityDisplaySetup: failed to take over the display");
        s_Display.reset();
        onComplete(false);
    }
}

#else

void TactilityDisplaySetup::Setup(SetupCallback onComplete)
{
    // Editor or a non-Tactility target: nothing to set up, and saying so keeps
    // the component inspectable on the desktop.
    onComplete(true);
}

#endif

}  // namespace DekiTactility
