#include "hosts/reshade/ngx_hook_api.h"
#include "hosts/reshade/shell_host.h"
#include "hosts/reshade/direct_host.h"
#include "hosts/reshade/model_host_ngx.h"
#include "hosts/reshade/readable_guides.h"
#include "hosts/reshade/menu_pipeline.h"
#include "hosts/reshade/menu_params.h"
#include "hosts/reshade/addon/addon_context.h"
#include "hosts/reshade/addon/config_store.h"
#include <detours.h>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
namespace ofps::reshade {
namespace {
HookStatus hookStatus;
std::mutex hookMutex;
// Never hold this lock while calling the core (callbacks take core -> shell).
std::mutex shellMutex;
// Correction C4 (menu mode plan): one call into a feature at a time. HookEvaluate holds it from the feature lookup
// through ModelHostNgx::BeginFrame, the core's evaluate and KeepBlock (so a host's params_, and menu mode's exit-reset
// wrapper, belong to one evaluate); HookRelease holds it (blocking: never on the present path) around removing the
// feature from the map and feature->Release(), so a release waits for an evaluate in progress and no evaluate uses a
// released feature. Menu mode's own call into a feature (Task 12, WithFeatureForMenu) takes it with try_to_lock and
// holds it through that call.
// Lock order: menu pipeline lock -> featureCallMutex -> core lock (shellMutex is only taken briefly, never around a
// call). Nothing called under it takes the pipeline lock: MenuHostEvaluate's constructor and Evaluated() and
// MenuFeatureReleased (which take it) run before or after it is held; MenuTracedEvaluate, which runs inside the core's
// evaluate, only takes the parameter book's leaf lock.
std::mutex featureCallMutex;
std::atomic<bool> enabled{true}, safeMode{false};
thread_local void *currentParams = nullptr;
std::unordered_map<void *, IOfpsFeature *> features;
std::unordered_map<void *, std::shared_ptr<ModelHostNgx>> modelHosts;
std::unordered_set<void *> foreign;
std::unordered_set<void *> lateDirectHandles;
// Handles HookRelease took out of the maps (under featureCallMutex), until a create returns the same address again. An
// evaluate that waited for featureCallMutex while its handle was released finds it here and fails instead of forwarding
// the released handle to NGX (menu mode fix round 3).
std::unordered_set<void *> releasedHandles;
void Log(bool warning, const char *fmt, ...) {
    char text[2048]; va_list args; va_start(args, fmt); std::vsnprintf(text, sizeof(text), fmt, args); va_end(args);
    Host().Log(warning ? OFPS_LOG_WARN : OFPS_LOG_INFO, text);
}
void ReportLateDirectHandle(void *handle) {
    bool late = false;
    {
        std::lock_guard lock(shellMutex);
        late = features.count(handle) != 0 && lateDirectHandles.insert(handle).second;
    }
    if (late)
        Log(true, "Optimizer FPS NGX hook: direct host appeared after ReShade created handle %p; forwarding it untouched", handle);
}
void SetReason(const char *fmt, ...) {
    va_list args; va_start(args, fmt); std::vsnprintf(hookStatus.reason, sizeof(hookStatus.reason), fmt, args); va_end(args);
}
bool LoadForwarder() {
    wchar_t path[MAX_PATH] = {}; GetModuleFileNameW(State().module, path, MAX_PATH);
    std::string error;
    if (ofps::reshade::LoadForwarder(std::filesystem::path(path).parent_path().wstring(), &error)) return true;
    SetReason("%s", error.c_str()); return false;
}
}
bool HasShellFeatures() { std::lock_guard lock(shellMutex); return !features.empty(); }
void *CurrentParams() { return currentParams; }
void ForgetModelHost(void *handle) { std::lock_guard lock(shellMutex); modelHosts.erase(handle); }
void SetHookReason(const char *text) { std::lock_guard lock(hookMutex); SetReason("%s", text ? text : ""); }
void SetEnabled(bool value) { enabled = value; }
void SetSafeMode(bool value) { safeMode = value; }
bool SafeMode() { return safeMode; }
HookStatus GetHookStatus() { std::lock_guard lock(hookMutex); return hookStatus; }
namespace {
int CreateHooked(ID3D12GraphicsCommandList *cmd, int id, void *params, void **out) {
    if (DirectHostActive()) {
        const int result = CallCreate(cmd, id, params, out);
        if (id == kFeatureNeuralRendering && result == kNgxSuccess && out && *out)
            Log(false, "Optimizer FPS NGX hook: handle %p owned by OptiScaler (direct event); create forwarded", *out);
        return result;
    }
    if (SafeMode() || !Core() || id != kFeatureNeuralRendering || !cmd || !params || !out)
        return CallCreate(cmd, id, params, out);
    currentParams = params;
    OfpsFeatureDesc desc{};
    desc.size = sizeof(desc);
    if (!GetUInt(params, "DLSSNR.Width", &desc.width) || !GetUInt(params, "DLSSNR.Height", &desc.height) ||
        !desc.width || !desc.height) {
        Log(true, "Optimizer FPS NGX hook: feature 18 create without DLSSNR.Width/Height; forwarded untouched");
        return CallCreate(cmd, id, params, out);
    }
    // Creation parameters have extents but need not contain any frame resources yet.
    if (FAILED(cmd->GetDevice(IID_PPV_ARGS(&desc.resourceDevice)))) {
        Log(true, "Optimizer FPS NGX hook: feature 18 create without command-list device; forwarded untouched");
        return CallCreate(cmd, id, params, out);
    }
    desc.adapterLuid = desc.resourceDevice->GetAdapterLuid();
    auto host = std::make_shared<ModelHostNgx>(params); host->BeginFrame(params, nullptr);
    host->SetEvaluate(&MenuTracedEvaluate); // menu mode (menu_params.h): forwards unless this thread evaluates the watched feature
    IOfpsFeature *feature = nullptr;
    { std::lock_guard lock(shellMutex); modelHosts.emplace(host.get(), host); }
    const int rc = Core()->CreateFeature(cmd, &desc, host.get(), &feature);
    desc.resourceDevice->Release();
    host->EndFrame(cmd, nullptr, nullptr);
    if (rc == OFPS_S_FOREIGN) {
        ForgetModelHost(host.get());
        Log(true, "Optimizer FPS NGX hook: another NR feature is alive in this process; this one is left to its host untouched (forwarded as-is) until it is the only one left");
        const int result = CallCreate(cmd, id, params, out);
        if (result == kNgxSuccess && *out) { std::lock_guard lock(shellMutex); foreign.insert(*out); }
        else Log(true, "Optimizer FPS NGX hook: the second consumer's feature 18 create returned %d (forwarded as-is)", result);
        ShellState().lastNgxResult = result; return result;
    }
    ShellState().lastNgxResult = host->LastNgxResult();
    if (rc < 0 || !feature) {
        Log(true, "Optimizer FPS NGX hook: feature 18 create failed (%d)", ShellState().lastNgxResult);
        // The core queued OFPS_EVENT_FEATURE_RELEASED for this host and delivers it once the graveyard no
        // longer references the host (it may still owe a ReleaseModel); that event removes the entry.
        return ShellState().lastNgxResult == kNgxSuccess ? 0 : ShellState().lastNgxResult;
    }
    *out = feature->CurrentModelHandle(); host->SetHostHandle(*out);
    {
        std::lock_guard lock(shellMutex);
        modelHosts.erase(host.get());
        features[*out] = feature; modelHosts[*out] = host;
    }
    Log(false, "Optimizer FPS NGX hook: handle %p owned by ReShade", *out);
    return ShellState().lastNgxResult;
}
} // namespace
int __cdecl HookCreate(ID3D12GraphicsCommandList *cmd, int id, void *params, void **out) {
    const int result = CreateHooked(cmd, id, params, out);
    // Only a successful create returned a handle: a failed one may leave the caller's old value in *out.
    if (result == kNgxSuccess && out && *out) { std::lock_guard lock(shellMutex); releasedHandles.erase(*out); } // a new feature at a released address
    return result;
}
int __cdecl HookEvaluate(ID3D12GraphicsCommandList *cmd, void *handle, void *params, void *callback) {
    // Every route, including direct-host forwarding, must feed NGX readable guides: another consumer
    // (renodx) may still evaluate with typed depth. The fork passes readable clones, so this is a no-op for it.
    ReadableGuides readableGuides(cmd, params);
    if (DirectHostActive()) {
        ReportLateDirectHandle(handle);
        return CallEvaluate(cmd, handle, params, callback);
    }
    if (SafeMode() || !Core() || !cmd || !params || !handle)
        return CallEvaluate(cmd, handle, params, callback);
    // Menu mode: this evaluate ends a menu run (resetting NR history after menu passes, after the host's NR queue was
    // ordered behind the last menu pass), and the watched feature's evaluate is traced and snapshotted (menu_pipeline.h).
    // Alive until the core's evaluate returned.
    MenuHostEvaluate menu(handle, params, cmd); // takes the pipeline lock itself, before featureCallMutex
    // C2: a menu pass that could not be ordered before this evaluate may still run: the model call is withheld.
    if (menu.Withheld()) { ShellState().lastNgxResult = kMenuWithheldResult; return kMenuWithheldResult; }
    currentParams = params;
    std::unique_lock call(featureCallMutex);
    IOfpsFeature *feature = nullptr;
    std::shared_ptr<ModelHostNgx> host;
    bool released = false;
    {
        std::lock_guard lock(shellMutex);
        released = releasedHandles.count(handle) != 0;
        const auto it = features.find(handle);
        if (!released && it != features.end()) { feature = it->second; host = modelHosts.at(handle); }
    }
    if (released) { // released while this evaluate waited for featureCallMutex: neither looked up nor forwarded
        call.unlock();
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true)) Log(true, "Optimizer FPS NGX hook: an evaluate of handle %p arrived after its release; failed (logged once)", handle);
        ShellState().lastNgxResult = kMenuWithheldResult;
        return kMenuWithheldResult;
    }
    if (!feature) {
        OfpsFeatureDesc desc{};
        if (!ReadFeatureDesc(params, &desc)) { call.unlock(); return CallEvaluate(cmd, handle, params, callback); }
        host = std::make_shared<ModelHostNgx>(params); host->SetHostHandle(handle); host->BeginFrame(params, callback);
        host->SetEvaluate(&MenuTracedEvaluate); // menu mode (menu_params.h): forwards unless this thread evaluates the watched feature
        {
            // A feature we declined earlier (another one was warped) gets its turn once it is the only one left.
            std::lock_guard lock(shellMutex);
            if (foreign.count(handle) != 0 && features.empty())
                Log(false, "Optimizer FPS NGX hook: the other NR feature is gone; the second consumer's feature is adopted now");
        }
        const int rc = Core()->AdoptFeature(cmd, &desc, handle, host.get(), &feature);
        desc.resourceDevice->Release();
        if (rc != OFPS_OK || !feature) {
            { std::lock_guard lock(shellMutex); foreign.insert(handle); }
            call.unlock();
            return CallEvaluate(cmd, handle, params, callback);
        }
        std::lock_guard lock(shellMutex);
        foreign.erase(handle); features[handle] = feature; modelHosts[handle] = host;
    }
    // The host's block, or after menu passes a wrapper answering DLSSNR.Reset=1 (C8): the core's frame and the model host
    // read it, so the core drops its history as well; the host's block keeps its keys. The model host gets the host's
    // block back after the evaluate.
    void *const block = menu.Params();
    host->BeginFrame(block, callback);
    OfpsFrameInputs frame{}; ReadFrameInputs(block, &frame, &ShellState().frame);
    OfpsEvalResult result{}; result.size = sizeof(result);
    feature->Evaluate(cmd, &frame, &result);
    host->KeepBlock(params);
    const bool modelCalled = host->ModelWasCalled();
    ShellState().lastNgxResult = modelCalled ? host->LastNgxResult() : kNgxSuccess;
    void *const currentModel = feature->CurrentModelHandle();
    call.unlock(); // Evaluated takes the pipeline lock, which comes before featureCallMutex
    menu.Evaluated(currentModel, modelCalled, ShellState().lastNgxResult == kNgxSuccess);
    return ShellState().lastNgxResult;
}
int __cdecl HookRelease(void *handle) {
    if (DirectHostActive()) {
        ReportLateDirectHandle(handle);
        return CallRelease(handle);
    }
    if (SafeMode() || !Core()) return CallRelease(handle);
    // Menu mode first, with the pipeline lock and before featureCallMutex (lock order above): the watched feature's
    // release ends menu mode's use of it, and any release makes an evaluate that began before it bind nothing.
    // False: a menu pass that may still use the model is not proven complete; the feature is then not released at all.
    // Until this function returns, an evaluate of the handle is withheld (menu mode's release tombstone).
    const bool releasable = MenuFeatureReleased(handle);
    struct ReleaseDone {
        void *handle;
        ~ReleaseDone() { MenuFeatureReleaseDone(handle); }
    } releaseDone{handle};
    bool wasForeign = false;
    IOfpsFeature *feature = nullptr;
    std::unique_lock call(featureCallMutex); // waits for an evaluate in progress
    {
        std::lock_guard lock(shellMutex);
        releasedHandles.insert(handle); // an evaluate waiting for featureCallMutex now fails instead of forwarding it
        wasForeign = foreign.erase(handle) != 0;
        const auto it = features.find(handle);
        if (it != features.end()) { feature = it->second; features.erase(it); }
    }
    if (wasForeign || !feature) {
        call.unlock();
        if (wasForeign) Core()->NotifyForeignReleased(handle);
        return CallRelease(handle);
    }
    if (!releasable) { // a broken GPU: the model (and the core's feature) are leaked rather than freed under a running pass
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true))
            Log(true, "Optimizer FPS NGX hook: handle %p is not released: a menu pass on its model did not finish (leaked for the session)", handle);
        return kNgxSuccess;
    }
    feature->Release();
    return kNgxSuccess;
}
bool WithFeatureForMenu(void *hostHandle, const std::function<bool(IOfpsFeature &, ModelHostNgx &)> &call) {
    std::unique_lock callLock(featureCallMutex, std::try_to_lock); // never waits: the present path
    if (!callLock) return false;
    IOfpsFeature *feature = nullptr;
    std::shared_ptr<ModelHostNgx> host;
    {
        std::lock_guard lock(shellMutex);
        if (releasedHandles.count(hostHandle) != 0) return false;
        const auto it = features.find(hostHandle);
        const auto hostIt = modelHosts.find(hostHandle);
        if (it == features.end() || hostIt == modelHosts.end()) return false;
        feature = it->second;
        host = hostIt->second;
    }
    return call(*feature, *host);
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
    Shim().realCreate = create;
    Shim().realEvaluate = evaluate;
    Shim().realRelease = release;
    // 26.26: every step of the transaction is checked and a failure aborts it - a half-built
    // transaction left open would take the next attempt's Begin down with it. The enlisted thread is
    // the current one only (the usual ReShade add-on practice: the add-on is loaded before the game
    // has a render thread inside nvngx_dlssnr.dll, and no thread is suspended here to enumerate).
    const LONG begin = DetourTransactionBegin();
    if (begin != NO_ERROR) {
        SetReason("Detours transaction begin failed (%ld)", begin);
        Shim().realCreate = nullptr;
        Shim().realEvaluate = nullptr;
        Shim().realRelease = nullptr;
        return false;
    }
    LONG step = DetourUpdateThread(GetCurrentThread());
    const char *what = "update thread";
    if (step == NO_ERROR) { what = "attach create"; step = DetourAttach(&reinterpret_cast<PVOID &>(Shim().realCreate), reinterpret_cast<PVOID>(&HookCreate)); }
    if (step == NO_ERROR) { what = "attach evaluate"; step = DetourAttach(&reinterpret_cast<PVOID &>(Shim().realEvaluate), reinterpret_cast<PVOID>(&HookEvaluate)); }
    if (step == NO_ERROR) { what = "attach release"; step = DetourAttach(&reinterpret_cast<PVOID &>(Shim().realRelease), reinterpret_cast<PVOID>(&HookRelease)); }
    if (step != NO_ERROR) {
        const LONG aborted = DetourTransactionAbort();
        SetReason("Detours %s failed (%ld; abort %ld)", what, step, aborted);
        Shim().realCreate = nullptr;
        Shim().realEvaluate = nullptr;
        Shim().realRelease = nullptr;
        return false;
    }
    const LONG error = DetourTransactionCommit();
    if (error != NO_ERROR) {
        SetReason("Detours commit failed (%ld)", error);
        Shim().realCreate = nullptr;
        Shim().realEvaluate = nullptr;
        Shim().realRelease = nullptr;
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
        Shim().realCreate = nullptr;
        Shim().realEvaluate = nullptr;
        Shim().realRelease = nullptr;
        SetReason("exception 0x%08lx while installing the nvngx_dlssnr.dll hooks", code);
    }
    return ok;
}

void Poll()
{
    if (!Core()) return;
    Core()->Housekeeping();
    // Checked before the lock: once the hooks are in, Poll may be reached from a ReShade event
    // raised inside an NGX call that already holds hookMutex (a queue created by the model).
    if (hookStatus.hooked || !enabled) return;
    OfpsSettingsValues values{}; values.size = sizeof(values); Core()->GetSettings(&values);
    std::lock_guard<std::mutex> lock(hookMutex);
    if (hookStatus.hooked) return;
    HMODULE snippet = GetModuleHandleW(L"nvngx_dlssnr.dll");
    if (snippet == nullptr) return;
    hookStatus.moduleFound = true;
    const ULONGLONG hookDelayMs = values.v[OFPS_SET_DEBUG_HOOK_DELAY_MS].i;
    if (hookDelayMs != 0 && !Shim().hookDelayLogged) {
        Shim().hookDelayLogged = true;
        Log(true, "Optimizer FPS NGX hook: DebugHookDelayMs=%llu (debug)", hookDelayMs);
    }
    if (Shim().hookFirstPoll == 0) Shim().hookFirstPoll = GetTickCount64();
    if (GetTickCount64() - Shim().hookFirstPoll < hookDelayMs) return;
    ++Shim().hookAttempts;
    hookStatus.hookAttempts = Shim().hookAttempts;
    if (Shim().hookAttempts == 1) Log(false, "Optimizer FPS NGX hook: installing the nvngx_dlssnr.dll hooks (module %p)", (void *) snippet);
    hookStatus.hooked = InstallHooks(snippet);
    if (hookStatus.hooked) {
        Log(false, "Optimizer FPS NGX hook: nvngx_dlssnr.dll feature 18 create/evaluate/release hooked (attempt %u)",
            Shim().hookAttempts);
        hookStatus.reason[0] = 0;
    } else if (Shim().hookAttempts == 1 || Shim().hookAttempts % 100 == 0) {
        Log(true, "Optimizer FPS NGX hook: %s (attempt %u; retried every present)", hookStatus.reason, Shim().hookAttempts);
    }
}


} // namespace ofps::reshade
