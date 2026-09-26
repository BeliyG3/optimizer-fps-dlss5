#include "shell_settings.h"
#include "ini_store.h"

#include <reshade.hpp>

namespace ofps::reshade {
namespace {
ShellSettings g_settings;
}

void LoadShellSettings() {
    g_settings = {};
    const char *section = ActiveIniSection();
    int value = 0;
    if (::reshade::get_config_value(nullptr, section, "Passive", value))
        g_settings.passive = value != 0;
    if (::reshade::get_config_value(nullptr, section, "FloatingWindow", value))
        g_settings.floatingWindow = value != 0;
    if (::reshade::get_config_value(nullptr, section, "TraceExit", value))
        g_settings.traceExit = value != 0;
    if (::reshade::get_config_value(nullptr, section, "DebugLayer", value)) {
        g_settings.debugLayer = value != 0;
        g_settings.debugLayerPresent = true;
    }
    if (::reshade::get_config_value(nullptr, section, "CrashGuard", value)) {
        g_settings.crashGuard = value != 0;
        g_settings.crashGuardPresent = true;
    }
}

const ShellSettings &CurrentShellSettings() { return g_settings; }

} // namespace ofps::reshade
