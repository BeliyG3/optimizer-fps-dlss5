#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <imgui.h>
#include <reshade.hpp>
#ifndef IMGUI_VERSION_NUM
#error "imgui.h must come before reshade.hpp: register_addon fills the ImGui function table only then"
#endif

#include "remote_link.h"
#include "remote_overlay.h"
#include "overlay_event.h"
#include "host_watch.h"
#include "ini_section.h"
#include "ofps_version.h"

extern "C" __declspec(dllexport) const char *NAME = "Optimizer FPS for DLSS5 (tab for the 64-bit host)";
extern "C" __declspec(dllexport) const char *DESCRIPTION =
    "Optimizer FPS for DLSS5 " OFPS_ADDON_VERSION_STRING
    ": settings of the 64-bit Neural Rendering host.";

namespace {
bool floatingWindow = false;
void OnPresent(reshade::api::effect_runtime *) {
    ofps::remote::Poll();
    ofps::remote::WatchHosts();
}
void DrawEmbedded(reshade::api::effect_runtime *) { ofps::remote::DrawOverlay(false); }
void DrawWindow(reshade::api::effect_runtime *) { ofps::remote::DrawOverlay(true); }
} // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        if (!reshade::register_addon(module)) return FALSE;
        reshade::register_event<reshade::addon_event::reshade_present>(OnPresent);
        ofps::remote::OverlayEventRegister();
        reshade::register_overlay(nullptr, DrawEmbedded);
        int floating = 0;
        if (reshade::get_config_value(nullptr, ofps::remote::ActiveIniSectionReadOnly(),
                                      "FloatingWindow", floating)) floatingWindow = floating != 0;
        if (floatingWindow) reshade::register_overlay("Optimizer FPS for DLSS5", DrawWindow);
    } else if (reason == DLL_PROCESS_DETACH) {
        if (floatingWindow) reshade::unregister_overlay("Optimizer FPS for DLSS5", DrawWindow);
        reshade::unregister_overlay(nullptr, DrawEmbedded);
        reshade::unregister_event<reshade::addon_event::reshade_present>(OnPresent);
        ofps::remote::OverlayEventUnregister();
        ofps::remote::SignalLeaving(reserved != nullptr);
        ofps::remote::Close();
        reshade::unregister_addon(module);
    }
    return TRUE;
}
