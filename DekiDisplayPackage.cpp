// Package entry point for deki-display.
#include "DekiDisplayPackage.h"
#include <deki/interop/Plugin.h>
#include <deki/LogSystem.h>

extern void DekiDisplayRegisterComponents();
extern int DekiDisplayGetAutoComponentCount();
extern const Deki::ComponentMeta* DekiDisplayGetAutoComponentMeta(int index);

namespace DekiDisplay
{

#ifdef DEKI_EDITOR
#endif

static bool s_DisplayRegistered = false;

}  // namespace DekiDisplay
// The exports below are C symbols at global scope; the package's own
// registration helpers and statics live in its namespace.
using namespace DekiDisplay;

extern "C"
{
    DEKI_DISPLAY_API int DekiDisplayEnsureRegistered(void)
    {
#ifdef DEKI_EDITOR
        if (s_DisplayRegistered)
        {
            return ::DekiDisplayGetAutoComponentCount();
        }
        s_DisplayRegistered = true;
        ::DekiDisplayRegisterComponents();
        return ::DekiDisplayGetAutoComponentCount();
#else
        return 0;
#endif
    }

    DEKI_PLUGIN_API const char* DekiPluginGetName(void)
    {
        return "Deki Display Package";
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
        s_DisplayRegistered = false;
    }

#ifdef DEKI_EDITOR
    DEKI_PLUGIN_API int DekiPluginGetComponentCount(void)
    {
        return ::DekiDisplayGetAutoComponentCount();
    }
    DEKI_PLUGIN_API const Deki::ComponentMeta* DekiPluginGetComponentMeta(int index)
    {
        return ::DekiDisplayGetAutoComponentMeta(index);
    }
#else
    DEKI_PLUGIN_API int DekiPluginGetComponentCount(void)
    {
        return 0;
    }
    DEKI_PLUGIN_API const Deki::ComponentMeta* DekiPluginGetComponentMeta(int)
    {
        return nullptr;
    }
#endif

    DEKI_PLUGIN_API void DekiPluginRegisterComponents(void)
    {
#ifdef DEKI_EDITOR
        DekiDisplayEnsureRegistered();
#endif
    }

}  // extern "C"
