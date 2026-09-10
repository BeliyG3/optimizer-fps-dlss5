#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>
#include <d3d12.h>
#include <detours.h>

#include "addon_context.h"
#include "config_store.h"
#include "crash_guard.h"
#include "layout_bridge.h"
#include "overlay.h"
#include "queue_events.h"
#include "remote_host.h"
#include "../ngx_hook.h"
#include "pw_version.h"

#include <cstdio>
#include <cstring>

// ReShade keys its DisabledAddons list on NAME, so NAME carries no version: a version inside it
// would silently re-enable the add-on for every user who had turned it off. The release number
// lives in DESCRIPTION and in the overlay banner (pw_version.h, generated from cmake/Version.cmake).
extern "C" __declspec(dllexport) const char *NAME = "Optimizer FPS for DLSS5";
extern "C" __declspec(dllexport) const char *DESCRIPTION =
    "Optimizer FPS for DLSS5 " PW_ADDON_VERSION_STRING
    " - Optimizer FPS: compresses the frame periphery before DLSS Neural Rendering and runs the model every N frames (temporal modes); the settings are below.";

namespace pw_addon {
namespace {

HANDLE g_reShadeOverlayEvent = nullptr;

HANDLE EnsureReShadeOverlayEvent()
{
    if (g_reShadeOverlayEvent == nullptr) {
        wchar_t name[96] = {};
        swprintf_s(name, L"Local\\DLSS5_ReShadeOverlay_%lu",
                   static_cast<unsigned long>(GetCurrentProcessId()));
        g_reShadeOverlayEvent = CreateEventW(nullptr, TRUE, FALSE, name);
    }
    return g_reShadeOverlayEvent;
}

bool OnReShadeOpenOverlay(reshade::api::effect_runtime *, bool open,
                          reshade::api::input_source)
{
    HANDLE eventHandle = EnsureReShadeOverlayEvent();
    if (eventHandle != nullptr) {
        if (open) SetEvent(eventHandle);
        else ResetEvent(eventHandle);
    }
    return false;
}

// DLL detach: the overlay-visibility event is the presenter's, and a stale signalled event would
// tell it the ReShade overlay is still open after this add-on is gone.
void CloseReShadeOverlayEvent()
{
    if (g_reShadeOverlayEvent != nullptr) {
        ResetEvent(g_reShadeOverlayEvent);
        CloseHandle(g_reShadeOverlayEvent);
        g_reShadeOverlayEvent = nullptr;
    }
}

void OnPresent(reshade::api::effect_runtime *)
{
    AddonState &state = State();
    FlushPendingQueues(); // 26.7.4
    LoadPersistedConfigOnce();
    ProbeLayoutBridge();
    PullLayoutFromBridge();
    // Legacy linked mode: OptiScaler warps inside its own NR stage and hooking the snippet on top of it
    // would warp twice. Takeover (default): OptiScaler's warp is forced Off and the hook does the work.
    pw_ngx::SetEnabled(!BridgeLinked() || state.optiTakeover);
    CrashGuardOnPresent();
    pw_ngx::Poll();
    pw_ngx::SetOutlines(state.showCenterOutline, state.showWorkOutline);
    pw_ngx::SetMotionAdjust(state.motionScaleAdjust, state.motionInvert);
    pw_ngx::SetOutputColorAdjust(state.brightnessPercent, state.gamma);
    pw_ngx::SetTemporal(state.temporal);
    // The remote tab is for the NGX-hook case (32-bit game, model in a 64-bit host); with a layout
    // bridge OptiScaler owns the layout and its own menu is visible.
    if (!BridgeLinked() || state.optiTakeover) RemotePublish();
}

// 26.13: the settings are drawn inside ReShade's Add-ons tab by default (register_overlay with a null
// title), like the other DLSS 5 add-ons, so the tab is found where users look for it and cannot get
// lost in a hidden dock node. [PeripheralWarp] FloatingWindow=1 adds the old separate window as well.
bool g_floatingWindow = false;

// ---- 26.7.4 exit trace (diagnostics)
using PFN_ExitProcess = void(WINAPI *)(UINT);
using PFN_TerminateProcess = BOOL(WINAPI *)(HANDLE, UINT);
PFN_ExitProcess g_realExitProcess = nullptr;
PFN_TerminateProcess g_realTerminateProcess = nullptr;
void LogStack(const char *what, UINT code)
{
    void *frames[48] = {};
    const USHORT n = CaptureStackBackTrace(1, 48, frames, nullptr);
    char line[512];
    std::snprintf(line, sizeof(line), "Optimizer FPS exit trace: %s(%u) called; %u frames follow", what, code, (unsigned) n);
    reshade::log::message(reshade::log::level::warning, line);
    for (USHORT i = 0; i < n; ++i) {
        HMODULE mod = nullptr;
        char name[MAX_PATH] = "?";
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR) frames[i], &mod) && mod) {
            GetModuleFileNameA(mod, name, sizeof(name));
            const char *base = strrchr(name, '\\'); if (base) memmove(name, base + 1, strlen(base));
        }
        std::snprintf(line, sizeof(line), "Optimizer FPS exit trace:   #%02u %s+0x%llx", (unsigned) i, name, mod ? (unsigned long long) ((char *) frames[i] - (char *) mod) : (unsigned long long) frames[i]);
        reshade::log::message(reshade::log::level::warning, line);
    }
}
LONG g_exitTraced = 0;
void WINAPI HookExitProcess(UINT code)
{
    if (InterlockedExchange(&g_exitTraced, 1) == 0) LogStack("ExitProcess", code); // once: the DLL detach that follows may call exit again
    g_realExitProcess(code);
}
BOOL WINAPI HookTerminateProcess(HANDLE h, UINT code)
{
    if (h == GetCurrentProcess() || GetProcessId(h) == GetCurrentProcessId()) LogStack("TerminateProcess", code);
    return g_realTerminateProcess(h, code);
}
void InstallExitTrace()
{
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    if (k32 == nullptr) return;
    g_realExitProcess = reinterpret_cast<PFN_ExitProcess>(GetProcAddress(k32, "ExitProcess"));
    g_realTerminateProcess = reinterpret_cast<PFN_TerminateProcess>(GetProcAddress(k32, "TerminateProcess"));
    if (g_realExitProcess == nullptr || g_realTerminateProcess == nullptr) return;
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&reinterpret_cast<PVOID &>(g_realExitProcess), reinterpret_cast<PVOID>(&HookExitProcess));
    DetourAttach(&reinterpret_cast<PVOID &>(g_realTerminateProcess), reinterpret_cast<PVOID>(&HookTerminateProcess));
    const LONG err = DetourTransactionCommit();
    reshade::log::message(err == NO_ERROR ? reshade::log::level::warning : reshade::log::level::error,
                          err == NO_ERROR ? "Optimizer FPS: exit trace armed ([PeripheralWarp] TraceExit=1)" : "Optimizer FPS: exit trace could not hook ExitProcess");
}
} // namespace
} // namespace pw_addon

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        pw_addon::State().module = module;
        DisableThreadLibraryCalls(module);
        if (!reshade::register_addon(module)) return FALSE;
        {
            // 26.7.2: [PeripheralWarp] DebugLayer=1 turns on the D3D12 debug layer before the game creates its device and
            // makes the NGX hook print the layer's messages after its passes (diagnostics for hangs/crashes; slow).
            // 26.7.4 diagnostics: [PeripheralWarp] TraceExit=1 logs who calls ExitProcess/TerminateProcess (module + offset
            // of every frame on the stack). Death Stranding DC closes itself 25 ms after its D3D12 device is created whenever
            // this add-on is loaded, without a crash record; this shows the caller.
            int traceExit = 0;
            if (reshade::get_config_value(nullptr, "PeripheralWarp", "TraceExit", traceExit) && traceExit != 0) pw_addon::InstallExitTrace();
            int debugLayer = 0;
            if (reshade::get_config_value(nullptr, "PeripheralWarp", "DebugLayer", debugLayer) && debugLayer != 0) {
                ID3D12Debug *dbg = nullptr;
                if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg))) && dbg) { dbg->EnableDebugLayer(); dbg->Release(); }
                reshade::log::message(reshade::log::level::warning, "Optimizer FPS: D3D12 debug layer enabled by [PeripheralWarp] DebugLayer=1 (diagnostics; expect a big slowdown)");
            }
            // 26.26: the hook's diagnostic switches, read once from read-only ini keys (never written back).
            pw_addon::LoadDiagnosticsFromReShadeIni(debugLayer != 0);
        }
        {
            wchar_t path[MAX_PATH]{};
            GetModuleFileNameW(module, path, MAX_PATH);
            if (wchar_t *slash = wcsrchr(path, L'\\')) *slash = 0;
            pw_ngx::Configure(&pw_addon::ConfigForNgxHook, &pw_addon::LogForNgxHook, path);
            pw_ngx::SetFirstWarpedCallback(&pw_addon::CrashMarkerOnFirstWarped);
            pw_addon::CrashGuardInit(path);
        }
        // 26.7.4 diagnostics: [PeripheralWarp] Passive=1 registers the add-on and nothing else (no events, no overlay):
        // tells a game that dies from our mere presence apart from one that dies from something we do.
        int passive = 0;
        if (reshade::get_config_value(nullptr, "PeripheralWarp", "Passive", passive) && passive != 0) {
            reshade::log::message(reshade::log::level::warning, "Optimizer FPS: Passive=1 - no events, no overlay, no hooks (diagnostics)");
            return TRUE;
        }
        reshade::register_event<reshade::addon_event::init_command_queue>(pw_addon::OnInitCommandQueue);
        reshade::register_event<reshade::addon_event::destroy_command_queue>(pw_addon::OnDestroyCommandQueue);
        reshade::register_event<reshade::addon_event::execute_command_list>(pw_addon::OnExecuteCommandList);
        reshade::register_event<reshade::addon_event::reshade_present>(pw_addon::OnPresent);
        reshade::register_event<reshade::addon_event::reshade_open_overlay>(pw_addon::OnReShadeOpenOverlay);
        reshade::register_overlay(nullptr, pw_addon::DrawOverlayEmbedded); // Add-ons tab (default)
        {
            int floating = 0;
            if (reshade::get_config_value(nullptr, pw_addon::kIniSection, "FloatingWindow", floating)) pw_addon::g_floatingWindow = floating != 0;
        }
        if (pw_addon::g_floatingWindow) reshade::register_overlay("Optimizer FPS for DLSS5", pw_addon::DrawOverlay);
    } else if (reason == DLL_PROCESS_DETACH) {
        pw_addon::CrashMarkerClear(); // a clean unload: the next session may warp
        if (pw_addon::g_floatingWindow) reshade::unregister_overlay("Optimizer FPS for DLSS5", pw_addon::DrawOverlay);
        reshade::unregister_overlay(nullptr, pw_addon::DrawOverlayEmbedded);
        reshade::unregister_event<reshade::addon_event::reshade_open_overlay>(pw_addon::OnReShadeOpenOverlay);
        reshade::unregister_event<reshade::addon_event::reshade_present>(pw_addon::OnPresent);
        reshade::unregister_event<reshade::addon_event::destroy_command_queue>(pw_addon::OnDestroyCommandQueue);
        reshade::unregister_event<reshade::addon_event::init_command_queue>(pw_addon::OnInitCommandQueue);
        reshade::unregister_addon(module);
        pw_addon::CloseReShadeOverlayEvent();
    }
    return TRUE;
}
