// The public surface of the feature-18 interposer (stage 27.D1): the Detours installation, the
// present-time poll that waits for nvngx_dlssnr.dll, and the pw_ngx:: setters and getters the
// add-on drives the hook with. Everything they touch lives in the modules under ngx/.

#include "../ngx_hook.h"

#include "async_scheduler.h"
#include "feature_state.h"
#include "hook_context.h"
#include "hook_dispatch.h"
#include "ngx_common.h"

#include <detours.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>

namespace pwhook {














bool LoadForwarder()
{
    std::string error;
    if (pwngx::LoadForwarder(Ctx().shaderDirectory, &error)) return true;
    SetReason("%s", error.c_str());
    return false;
}

bool InstallHooksImpl(HMODULE snippet)
{
    if (!LoadForwarder()) return false;
    auto create = reinterpret_cast<PFN_Create>(GetProcAddress(snippet, "NVSDK_NGX_D3D12_CreateFeature"));
    auto evaluate = reinterpret_cast<PFN_Evaluate>(GetProcAddress(snippet, "NVSDK_NGX_D3D12_EvaluateFeature"));
    auto release = reinterpret_cast<PFN_Release>(GetProcAddress(snippet, "NVSDK_NGX_D3D12_ReleaseFeature"));
    if (create == nullptr || evaluate == nullptr || release == nullptr) {
        SetReason("nvngx_dlssnr.dll lacks the expected D3D12 exports");
        return false;
    }
    Ctx().realCreate = create;
    Ctx().realEvaluate = evaluate;
    Ctx().realRelease = release;
    // 26.26: every step of the transaction is checked and a failure aborts it - a half-built
    // transaction left open would take the next attempt's Begin down with it. The enlisted thread is
    // the current one only (the usual ReShade add-on practice: the add-on is loaded before the game
    // has a render thread inside nvngx_dlssnr.dll, and no thread is suspended here to enumerate).
    const LONG begin = DetourTransactionBegin();
    if (begin != NO_ERROR) {
        SetReason("Detours transaction begin failed (%ld)", begin);
        Ctx().realCreate = nullptr;
        Ctx().realEvaluate = nullptr;
        Ctx().realRelease = nullptr;
        return false;
    }
    LONG step = DetourUpdateThread(GetCurrentThread());
    const char *what = "update thread";
    if (step == NO_ERROR) { what = "attach create"; step = DetourAttach(&reinterpret_cast<PVOID &>(Ctx().realCreate), reinterpret_cast<PVOID>(&HookCreate)); }
    if (step == NO_ERROR) { what = "attach evaluate"; step = DetourAttach(&reinterpret_cast<PVOID &>(Ctx().realEvaluate), reinterpret_cast<PVOID>(&HookEvaluate)); }
    if (step == NO_ERROR) { what = "attach release"; step = DetourAttach(&reinterpret_cast<PVOID &>(Ctx().realRelease), reinterpret_cast<PVOID>(&HookRelease)); }
    if (step != NO_ERROR) {
        const LONG aborted = DetourTransactionAbort();
        SetReason("Detours %s failed (%ld; abort %ld)", what, step, aborted);
        Ctx().realCreate = nullptr;
        Ctx().realEvaluate = nullptr;
        Ctx().realRelease = nullptr;
        return false;
    }
    const LONG error = DetourTransactionCommit();
    if (error != NO_ERROR) {
        SetReason("Detours commit failed (%ld)", error);
        Ctx().realCreate = nullptr;
        Ctx().realEvaluate = nullptr;
        Ctx().realRelease = nullptr;
        return false;
    }
    return true;
}

// 26.7.4: the installation runs under a structured-exception guard. Death Stranding DC exited silently
// 50 ms after the snippet was loaded - an exception inside the hook installation caught by the game's
// own handler, which calls ExitProcess without a crash report. A failed installation is now a logged
// reason and a retry on the next present, never the end of the process.
bool InstallHooks(HMODULE snippet)
{
    bool ok = false;
    unsigned long code = 0;
    __try {
        ok = InstallHooksImpl(snippet);
    } __except (code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    if (code != 0) {
        Ctx().realCreate = nullptr;
        Ctx().realEvaluate = nullptr;
        Ctx().realRelease = nullptr;
        SetReason("exception 0x%08lx while installing the nvngx_dlssnr.dll hooks", code);
    }
    return ok;
}

} // namespace pwhook

namespace pw_ngx {

using namespace pwhook;

void Configure(GetConfigFn getConfig, LogFn log, const wchar_t *shaderDirectory)
{
    Ctx().getConfig = getConfig;
    pwngx::SetLog(log);
    Ctx().shaderDirectory = shaderDirectory != nullptr ? shaderDirectory : L".";
}

void SetEnabled(bool enabled) { Ctx().enabled = enabled; }
void SetSafeMode(bool safe) { Ctx().safeMode = safe; }
void SetFirstWarpedCallback(FirstWarpedFn callback) { Ctx().firstWarpedCallback.store(callback); }
bool SafeMode() { return Ctx().safeMode; }

void Poll()
{
    // Checked before the lock: once the hooks are in, Poll may be reached from a ReShade event
    // raised inside an NGX call that already holds Ctx().mutex (a queue created by the model).
    if (Ctx().status.hooked || !Ctx().enabled) return;
    std::lock_guard<std::mutex> lock(Ctx().mutex);
    if (Ctx().status.hooked) return;
    HMODULE snippet = GetModuleHandleW(L"nvngx_dlssnr.dll");
    if (snippet == nullptr) return;
    Ctx().status.moduleFound = true;
    const ULONGLONG hookDelayMs = Ctx().diag.hookDelayMs;
    if (hookDelayMs != 0 && !Ctx().hookDelayLogged) {
        Ctx().hookDelayLogged = true;
        Log(true, "Optimizer FPS NGX hook: DebugHookDelayMs=%llu (debug)", hookDelayMs);
    }
    if (Ctx().hookFirstPoll == 0) Ctx().hookFirstPoll = GetTickCount64();
    if (GetTickCount64() - Ctx().hookFirstPoll < hookDelayMs) return;
    ++Ctx().hookAttempts;
    Ctx().status.hookAttempts = Ctx().hookAttempts;
    if (Ctx().hookAttempts == 1) Log(false, "Optimizer FPS NGX hook: installing the nvngx_dlssnr.dll hooks (module %p)", (void *) snippet);
    Ctx().status.hooked = InstallHooks(snippet);
    if (Ctx().status.hooked) {
        Log(false, "Optimizer FPS NGX hook: nvngx_dlssnr.dll feature 18 create/evaluate/release hooked (attempt %u)",
            Ctx().hookAttempts);
        Ctx().status.reason[0] = 0;
    } else if (Ctx().hookAttempts == 1 || Ctx().hookAttempts % 100 == 0) {
        Log(true, "Optimizer FPS NGX hook: %s (attempt %u; retried every present)", Ctx().status.reason, Ctx().hookAttempts);
    }
}

void SetDiagnostics(const DiagnosticsConfig &config) { Ctx().diag = config; }

void SetOutlines(bool center, bool work)
{
    Ctx().showCenterOutline = center;
    Ctx().showWorkOutline = work;
}

void SetMotionAdjust(float scale, bool invert)
{
    Ctx().motionScaleAdjust = (std::isfinite(scale) && scale != 0.0f) ? scale : 1.0f;
    Ctx().motionInvert = invert;
}

void SetOutputColorAdjust(float brightnessPercent, float gamma)
{
    const float gain = 1.0f + brightnessPercent * 0.01f;
    Ctx().outputGain = (std::isfinite(gain) && gain > 0.0f) ? gain : 1.0f;
    Ctx().outputGamma = (std::isfinite(gamma) && gamma > 0.0f) ? gamma : 1.0f;
}

void SetTemporal(const TemporalSettings &settings)
{
    std::lock_guard<std::mutex> lock(Ctx().mutex);
    TemporalSettings s = settings;
    s.mode = std::clamp(s.mode, 0, 3);
    s.every = std::clamp(s.every, s.mode == 3 ? 1 : 2, 8);
    s.maxAge = std::clamp(s.maxAge, 3, 16);
    s.maxQueue = std::clamp(s.maxQueue, 0, 8);
    if (!std::isfinite(s.depthTolerance)) s.depthTolerance = 0.05f;
    s.depthTolerance = std::clamp(s.depthTolerance, 0.0f, 1.0f);
    if (!std::isfinite(s.colorTolerance)) s.colorTolerance = 0.08f;
    s.colorTolerance = std::clamp(s.colorTolerance, 0.0f, 1.0f);
    if (!std::isfinite(s.mvSearchRadiusPx)) s.mvSearchRadiusPx = 16.0f;
    s.mvSearchRadiusPx = std::clamp(s.mvSearchRadiusPx, 0.0f, 128.0f);
    const bool modeChanged = s.mode != Ctx().temporal.mode;
    Ctx().temporal = s;
    if (modeChanged) {
        for (auto &entry : Ctx().features) {
            entry.second->temporalDisabled = false;
            entry.second->temporalLogged = false;
            if (entry.second->temporal) entry.second->temporal->Invalidate();
            // Leaving the background mode: the pass in flight may still use the adapter's textures; its
            // release waits for it (bury it if the GPU is busy: same rule as the other GPU objects).
            if (entry.second->async && s.mode != 3) {
                pwngx::GateSet gate;
                BuryAsync(*entry.second, &gate);
            }
        }
        if (s.mode == 0) Ctx().status.temporalMode = 0;
    }
}

void OnCommandListExecuted(ID3D12CommandQueue *queue, ID3D12GraphicsCommandList *list)
{
    pwhook::OnCommandListExecuted(queue, list);
}

Status GetStatus()
{
    std::lock_guard<std::mutex> lock(Ctx().mutex);
    return Ctx().status;
}

void RegisterQueue(ID3D12Device *device, ID3D12CommandQueue *queue) { pwngx::RegisterQueue(device, queue); }

void UnregisterQueue(ID3D12CommandQueue *queue) { pwngx::UnregisterQueue(queue); }

} // namespace pw_ngx
