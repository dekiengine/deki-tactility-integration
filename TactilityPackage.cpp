// Package entry point for deki-tactility. Exports the standard Deki plugin
// interface, so the editor can load deki-tactility.dll and inspect the
// Tactility SetupComponents on a desktop, where none of them can run.
//
// Display, input and the filesystem are set up by their SetupComponents from
// the platform's boot scene.

#include "TactilityPackage.h"

#include <deki/interop/Plugin.h>
#include <deki/reflection/ComponentFactory.h>
#include <deki/reflection/ComponentRegistry.h>

extern void DekiTactilityRegisterComponents();
extern int DekiTactilityGetAutoComponentCount();
extern const Deki::ComponentMeta* DekiTactilityGetAutoComponentMeta(int index);

namespace DekiTactility
{

#ifdef DEKI_EDITOR

static bool s_TactilityRegistered = false;

}  // namespace DekiTactility

// The exports below are C symbols at global scope; the package's own
// registration helpers and statics live in its namespace.
using namespace DekiTactility;

extern "C"
{
    /// Registers the deki-tactility components, if not done yet.
    DEKI_TACTILITY_API int DekiTactilityEnsureRegistered(void)
    {
        if (s_TactilityRegistered)
        {
            return ::DekiTactilityGetAutoComponentCount();
        }
        s_TactilityRegistered = true;

        ::DekiTactilityRegisterComponents();

        return ::DekiTactilityGetAutoComponentCount();
    }

    // =============================================================================
    // Plugin metadata (for dynamic loading compatibility)
    // =============================================================================

    DEKI_PLUGIN_API const char* DekiPluginGetName(void)
    {
        return "Deki Tactility Package";
    }

    DEKI_PLUGIN_API const char* DekiPluginGetVersion(void)
    {
#ifdef DEKI_PACKAGE_VERSION
        return DEKI_PACKAGE_VERSION;
#else
        return "0.0.0-dev";
#endif
    }

    DEKI_PLUGIN_API int DekiPluginInit(void)
    {
        return 0;
    }

    DEKI_PLUGIN_API void DekiPluginShutdown(void)
    {
        s_TactilityRegistered = false;
    }

    DEKI_PLUGIN_API int DekiPluginGetComponentCount(void)
    {
        return ::DekiTactilityGetAutoComponentCount();
    }

    DEKI_PLUGIN_API const Deki::ComponentMeta* DekiPluginGetComponentMeta(int index)
    {
        return ::DekiTactilityGetAutoComponentMeta(index);
    }

    DEKI_PLUGIN_API void DekiPluginRegisterComponents(void)
    {
        DekiTactilityEnsureRegistered();
    }

    DEKI_TACTILITY_API const char* DekiTactilityGetName(void)
    {
        return "Tactility";
    }

}  // extern "C"

#else  // !DEKI_EDITOR — runtime

// Components register through the generated
// ::DekiTactilityRegisterComponents(), which DekiRegisterProjectPackages()
// calls. Display, input and the filesystem are boot SetupComponents.

}  // namespace DekiTactility

#endif  // DEKI_EDITOR
