#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <reshade.hpp>

#include "overlay_event.h"

#include <cstdio>

namespace ofps::remote {
namespace {

HANDLE g_event = nullptr;

HANDLE EnsureEvent()
{
    if (g_event == nullptr) {
        wchar_t name[96] = {};
        swprintf_s(name, L"Local\\DLSS5_ReShadeOverlay_%lu", static_cast<unsigned long>(GetCurrentProcessId()));
        g_event = CreateEventW(nullptr, TRUE, FALSE, name);
    }
    return g_event;
}

bool OnOpenOverlay(reshade::api::effect_runtime *, bool open, reshade::api::input_source)
{
    HANDLE handle = EnsureEvent();
    if (handle != nullptr) {
        if (open) SetEvent(handle);
        else ResetEvent(handle);
    }
    return false; // we only observe; ReShade still opens and closes its overlay
}

} // namespace

void OverlayEventRegister()
{
    reshade::register_event<reshade::addon_event::reshade_open_overlay>(OnOpenOverlay);
}

void OverlayEventUnregister()
{
    reshade::unregister_event<reshade::addon_event::reshade_open_overlay>(OnOpenOverlay);
    if (g_event != nullptr) {
        ResetEvent(g_event);
        CloseHandle(g_event);
        g_event = nullptr;
    }
}

} // namespace ofps::remote
