#include "TactilityDisplaySetup.h"

#include <deki/LogSystem.h>

#if !defined(DEKI_EDITOR) && defined(DEKI_PACKAGE_TACTILITY)
#include <memory>

#include <deki/Engine.h>

#include "TactilityDisplay.h"
#include "TactilityWindowDisplay.h"
#endif

namespace DekiTactility
{

#if !defined(DEKI_EDITOR) && defined(DEKI_PACKAGE_TACTILITY)

// The package owns the display's lifetime; engine-core only holds the pointer.
static std::unique_ptr<Deki::IDisplay> s_Display;

void TactilityDisplaySetup::Setup(SetupCallback onComplete)
{
    const bool window = (mode == TactilityDisplayMode::Window);
    if (window)
        s_Display = std::make_unique<TactilityWindowDisplay>();
    else
        s_Display = std::make_unique<TactilityDisplay>();

    // 0x0 means "no expectation": on Tactility the OS owns the panel and
    // reports its real resolution, so there is nothing useful to assert here.
    // (The project's baked target size lives in project_data.bin, which the engine
    // reads during boot — not reliably before this component runs.)
    if (s_Display && s_Display->Initialize(0, 0))
    {
        Deki::Engine::GetInstance().SetDisplay(s_Display.get(), "Tactility");
        DEKI_LOG_INFO("TactilityDisplaySetup: display ready (%s)", window ? "window" : "panel");
        onComplete(true);
    }
    else
    {
        DEKI_LOG_ERROR("TactilityDisplaySetup: failed to set up the display (%s)", window ? "window" : "panel");
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
