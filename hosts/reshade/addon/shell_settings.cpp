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
    if (::reshade::get_config_value(nullptr, section, "DebugMenuPass", value)) {
        if (value >= 0 && value <= 3) g_settings.debugMenuPass = static_cast<MenuPassKind>(value);
        else ::reshade::log::message(::reshade::log::level::warning, "Optimizer FPS: DebugMenuPass must be 0..3; ignored (0)");
    }
    if (::reshade::get_config_value(nullptr, section, "DebugMenuDump", value))
        g_settings.debugMenuDump = value != 0 ? 1 : 0;
    if (::reshade::get_config_value(nullptr, section, "DebugMenuEntryMs", value) && value >= 0 && value <= 10000)
        g_settings.debugMenuEntryMs = value;
    if (::reshade::get_config_value(nullptr, section, "DebugMenuOwnBlock", value))
        g_settings.debugMenuOwnBlock = value != 0 ? 1 : 0;
    if (::reshade::get_config_value(nullptr, section, "DebugMenuNoFlow", value)) {
        if (value >= 0 && value <= 3) g_settings.debugMenuNoFlow = value;
        else ::reshade::log::message(::reshade::log::level::warning, "Optimizer FPS: DebugMenuNoFlow must be 0..3; ignored (0)");
    }
    if (::reshade::get_config_value(nullptr, section, "DebugMenuBridgeCanary", value))
        g_settings.debugMenuBridgeCanary = value != 0 ? 1 : 0;
    if (::reshade::get_config_value(nullptr, section, "CrashGuard", value)) {
        g_settings.crashGuard = value != 0;
        g_settings.crashGuardPresent = true;
    }
}

const ShellSettings &CurrentShellSettings() { return g_settings; }

} // namespace ofps::reshade
