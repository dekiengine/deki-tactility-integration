/**
 * @file TactilityPackage.cpp
 * @brief Package entry point for deki-tactility
 *
 * Exports the standard Deki plugin interface so the editor can load
 * deki-tactility.dll and keep the Tactility SetupComponents inspectable on a
 * desktop, even though nothing here can run there.
 *
 * Display, input and the filesystem are brought up by their SetupComponents
 * from the platform's boot scene.
 */

#include "TactilityPackage.h"

#include <deki/interop/Plugin.h>
#include <deki/reflection/ComponentFactory.h>
#include <deki/reflection/ComponentRegistry.h>

extern void DekiTactility_RegisterComponents();
extern int DekiTactility_GetAutoComponentCount();
extern const Deki::ComponentMeta* DekiTactility_GetAutoComponentMeta(int index);

namespace DekiTactility
{

#ifdef DEKI_EDITOR

static bool s_TactilityRegistered = false;

}  // namespace DekiTactility

// The exports below are C symbols at global scope; the package's own
// registration helpers and statics live in its namespace.
using namespace DekiTactility;

extern "C" {

/**
 * @brief Ensure the deki-tactility package's components are registered
 */
DEKI_TACTILITY_API int DekiTactility_EnsureRegistered(void)
{
    if (s_TactilityRegistered)
        return ::DekiTactility_GetAutoComponentCount();
    s_TactilityRegistered = true;

    ::DekiTactility_RegisterComponents();

    return ::DekiTactility_GetAutoComponentCount();
}

// =============================================================================
// Plugin metadata (for dynamic loading compatibility)
// =============================================================================

DEKI_PLUGIN_API const char* DekiPlugin_GetName(void)
{
    return "Deki Tactility Package";
}

DEKI_PLUGIN_API const char* DekiPlugin_GetVersion(void)
{
#ifdef DEKI_PACKAGE_VERSION
    return DEKI_PACKAGE_VERSION;
#else
    return "0.0.0-dev";
#endif
}

DEKI_PLUGIN_API int DekiPlugin_Init(void)
{
    return 0;
}

DEKI_PLUGIN_API void DekiPlugin_Shutdown(void)
{
    s_TactilityRegistered = false;
}

DEKI_PLUGIN_API int DekiPlugin_GetComponentCount(void)
{
    return ::DekiTactility_GetAutoComponentCount();
}

DEKI_PLUGIN_API const Deki::ComponentMeta* DekiPlugin_GetComponentMeta(int index)
{
    return ::DekiTactility_GetAutoComponentMeta(index);
}

DEKI_PLUGIN_API void DekiPlugin_RegisterComponents(void)
{
    DekiTactility_EnsureRegistered();
}

DEKI_TACTILITY_API const char* DekiTactility_GetName(void)
{
    return "Tactility";
}

}  // extern "C"

#else  // !DEKI_EDITOR — runtime

// Component registration happens through the auto-generated
// ::DekiTactility_RegisterComponents(), called by
// deki_register_project_packages(). Display, input and the filesystem come up
// as boot SetupComponents.

}  // namespace DekiTactility

#endif  // DEKI_EDITOR
