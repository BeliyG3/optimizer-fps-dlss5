#include "hosts/reshade/menu_pipeline.h"
#include "hosts/reshade/menu_pipeline_state.h"
#include "hosts/reshade/menu_core_pass.h"
#include "hosts/reshade/menu_dump.h"
#include "hosts/reshade/menu_fence.h"
#include "hosts/reshade/menu_params.h"
#include "hosts/reshade/menu_settings.h"
#include "hosts/reshade/ngx_params.h"
#include "hosts/reshade/addon/queue_events.h"
#include <memory>

// Menu mode on the host's side: the watched feature, the end of a run and the exit reset at its host evaluates, the host
// queue's exit order, and the proof that menu passes finished before the core may free the watched feature's model.
// The core's retirement gates are keyed on ReShade's proxy device while the private queue is registered natively
// (menu_pipeline_gpu.cpp), so they do not name it (Task 8 fix round 2, rulings A-C): the model may be freed only inside a
// host evaluate of the feature (a re-creation) or at its release, and at both points menu mode proves on the CPU, off the
// present path and without the pipeline's lock, that no pass may still use it (menu_outstanding.h):
//   - host evaluate: before featureCallMutex and the model call, at most 100 ms; a timeout withholds the evaluate (NGX
//     failure code) and the following ones, without waiting again, until the pass completed (final review I2: a slow
//     GPU keeps menu mode); only a removed device quarantines menu mode for the session;
//   - release: at most 500 ms; without a proof HookRelease does not release the feature at all (leaked); meanwhile a
//     tombstone withholds any evaluate of that handle.
namespace ofps::reshade {
using namespace menu_detail;
namespace {
// The last menu pass the exit evaluate's model call must follow (copied under the pipeline's lock, used without it).
struct LastPass {
    ComPtr<ID3D12Fence> f2;
    UINT64 value = 0;
    ComPtr<ID3D12CommandQueue> presentQueue;
};

// The watched feature: the first one whose evaluate succeeded, until it is released. The private queue goes on the device
// the host's list answers with (NGX's CUDA launch refuses lists of any other device); the colour's device is only logged.
bool Remember(Pipeline &p, void *hostHandle, void *params, ID3D12GraphicsCommandList *hostList) {
    ID3D12Device *device = nullptr;
    if (!hostList || FAILED(hostList->GetDevice(IID_PPV_ARGS(&device)))) return false;
    device->Release(); // the host keeps its device alive
    if ((p.gpu || p.bridge) && device != p.proxy) return false; // the pipeline stays on the device it was built for
    if (p.remembered) return p.hostHandle == hostHandle;
    ID3D12Resource *colour = GetResource(params, "DLSSNR.Color"), *output = GetResource(params, "DLSSNR.Output");
    ID3D12Device *colourDevice = nullptr, *outputDevice = nullptr; // the output's is the core's realDevice (the gates' key)
    if (colour && SUCCEEDED(colour->GetDevice(IID_PPV_ARGS(&colourDevice)))) colourDevice->Release();
    if (output && SUCCEEDED(output->GetDevice(IID_PPV_ARGS(&outputDevice)))) outputDevice->Release();
    Log("menu mode: watching feature %p; private queue on the host list's device %p (NGX's), which %s the NR colour's device %p (output's %p)",
        hostHandle, (void *) device, device == colourDevice ? "equals" : "DIFFERS FROM", (void *) colourDevice, (void *) outputDevice);
    p.proxy = device; p.hostHandle = hostHandle; p.remembered = true;
    p.listOnWrapped = !colourDevice || device == colourDevice;
    g_watching.store(true);
    return true;
}

// A host evaluate of the watched feature: the state machine's exit (C5: EndRun "exit"), and the reset owed by any run
// that submitted a pass. True: this evaluate resets NR history; *last is then the pass its model call must follow.
// HostEvaluated ends a run under the lock, so no pass is submitted after this point until the next entry (>= 150 ms).
bool AtHostEvaluate(Pipeline &p, LastPass *last) {
    if (p.state.HostEvaluated(NowMs())) {
        p.afterExit = kDumpAfterExit;
        EndRun(p, p.presents, "exit");
    }
    p.hostQueueWaiting = false; // the game runs NR again: a menu's queue blocker is over
    MenuCorePassHostEvaluated(); // a model the core owed (a refused creation in a menu) is created by this evaluate
    if (!p.state.TakeResetOwed()) return false;
    // The last pass whose f2 Signal succeeded, whatever happened to the GPU objects since (fix round 3).
    if (ID3D12Fence *f2 = p.outstanding.LastFence()) {
        last->f2 = f2;
        last->value = p.outstanding.LastValue();
        if (p.gpu) last->presentQueue = p.gpu->presentQueue;
    }
    return true;
}

// The pass the watched feature's model call or release must follow: the outstanding one, else the last while unfinished
// (menu_outstanding.h keeps both apart from the GPU objects: a poisoned or drained Gpu never hides them).
MenuPassProof ProofOf(Pipeline &p, bool release) { return p.outstanding.ProofFor(release); }

// Final review (Claude I2): a menu pass still running after the 100 ms bound on a live device is a slow GPU (a menu pass
// of 400-600 ms on a VRAM-starved card), not a broken one. It stays outstanding: this and the following evaluates of the
// feature are withheld without waiting until it completed, then run again; menu mode stays on. Under the lock.
void LogSlow(Pipeline &p, void *hostHandle) {
    p.loggedWithhold = true;
    Log("menu mode: a menu pass still ran 100 ms into the game's NR evaluate of feature %p (a slow GPU); that evaluate and the "
        "following ones are withheld (NGX failure code) until it completed, menu mode stays on", hostHandle);
}

// C2, before the model call, without the lock: the host's NR queue waits for the last menu pass (a GPU Wait, else a CPU
// wait of at most 100 ms). False: the pass did not complete; the model call is withheld (quarantine only for a removed
// device).
bool OrderExit(void *hostHandle, ID3D12GraphicsCommandList *list, const LastPass &last) {
    const MenuExitOrder order = HostQueue().OrderExitNow(list, last.f2.Get(), last.value, last.presentQueue.Get());
    if (Dumping()) {
        static constexpr const char *kOrder[] = {"nothing to order", "a GPU wait on the host's NR queue", "a CPU wait (passed)", "NOT COMPLETE"};
        Log("menu mode: the exit evaluate follows the last menu pass (f2 %llu): %s", (unsigned long long) last.value, kOrder[int(order)]);
    }
    if (order != MenuExitOrder::Quarantine) return true;
    const bool removed = MenuFenceRemoved(last.f2.Get());
    std::lock_guard lock(PipelineMutex());
    auto &p = P();
    MenuPassProof slow;
    slow.fence = last.f2;
    slow.value = last.value;
    slow.outstanding = true;
    // Held and expired: the following evaluates withhold without waiting again, until it completed.
    p.outstanding.Settle(slow, removed ? MenuPassWait::Removed : MenuPassWait::TimedOut);
    if (!removed) { LogSlow(p, hostHandle); return false; }
    p.loggedWithhold = true;
    Quarantine(p, p.presents, "the device was removed before the last menu pass was proven finished; the game's NR evaluates are "
                              "withheld (NGX failure code)");
    return false;
}

// Ruling A, before the model call, without the lock: the model is called only once the pass is proven complete (at most
// 100 ms, and no wait at all for a pass that was already too slow once). False: withhold this evaluate.
bool Proven(void *hostHandle, const MenuPassProof &proof) {
    const MenuPassWait result = MenuProvePass(proof, kMenuExitCpuWaitMs);
    std::lock_guard lock(PipelineMutex());
    auto &p = P();
    switch (p.outstanding.Settle(proof, result)) {
    case MenuPassGate::Call: return true;
    case MenuPassGate::Resumed:
        p.loggedWithhold = false;
        Log("menu mode: the outstanding menu pass completed; the game's NR evaluates of feature %p run again", hostHandle);
        return true;
    case MenuPassGate::Slow:
        LogSlow(p, hostHandle);
        return false;
    case MenuPassGate::Quarantine:
        p.loggedWithhold = true;
        Quarantine(p, p.presents, "the device was removed before a menu pass was proven finished; the game's NR evaluates are "
                                  "withheld (NGX failure code)");
        return false;
    case MenuPassGate::Withhold: break;
    }
    if (!p.loggedWithhold) {
        p.loggedWithhold = true;
        Log("menu mode: the game's NR evaluate of feature %p is withheld (NGX failure code): %s", hostHandle,
            p.outstanding.Unfenced() ? "a menu pass that was never fenced may still run (for the rest of the session)"
                                     : "a menu pass may still run (until it completed; logged once)");
    }
    return false;
}

// Under the lock. False: not the watched feature. *proof: what the core's release of it must follow.
bool ReleaseWatched(Pipeline &p, void *hostHandle, MenuPassProof *proof) {
    MenuBook().Clear(hostHandle); // every release, also a feature that was traced but never became the watched one
    if (!p.remembered || p.hostHandle != hostHandle) return false;
    g_releasing.Set(hostHandle); // ruling C: before the guards below go, until HookRelease has finished
    *proof = ProofOf(p, true);
    p.outstanding.Clear();
    if (p.state.Active()) EndRun(p, p.presents, "release");
    p.state.Forget();
    p.remembered = false; p.hostHandle = nullptr; p.snapshotProblem = nullptr; p.hostQueueWaiting = false;
    g_watching.store(false);
    HostQueue().Forget();
    MenuModelBind(nullptr);
    MenuGuidesRelease();
    if (p.gpu) {
        Log("menu pipeline: the watched feature is released with the private queue's last pass at f2 %llu (completed %llu)",
            (unsigned long long) p.gpu->v2, (unsigned long long) p.gpu->f2->GetCompletedValue());
        if (p.gpu->resources) RetireResources(*p.gpu);
    }
    if (p.bridge) ForgetD3D11(p); // logs the bridge's last pass (C1 evidence, as the D3D12 line above)
    double gpuAvg = 0, gpuMax = 0; unsigned gpuSamples = 0;
    MenuGpuTimeTotals(&gpuAvg, &gpuMax, &gpuSamples);
    Log("menu mode: feature %p released at present %llu. Totals: %u runs, %llu menu presents, %llu refused, %llu skipped, %llu shown again; cpu avg %.4f ms, max %.4f ms; gpu avg %.3f ms, max %.3f ms over %u",
        hostHandle, p.presents, p.runs, p.menuPresents, p.failures, p.skips, p.reused, p.menuPresents ? p.cpuTotal / double(p.menuPresents) : 0.0,
        p.cpuTotalMax, gpuAvg, gpuMax, gpuSamples);
    const double n = p.menuPresents ? double(p.menuPresents) : 1.0;
    MenuEvent("totals %llu %llu %.4f %.4f %.3f %.3f %.4f %.4f", p.menuPresents, p.failures, p.cpuTotal / n, p.cpuTotalMax, gpuAvg, gpuMax,
              p.pipeTotal / n, p.pipeTotalMax);
    return true;
}

// Ruling C: an evaluate of the watched feature while its release is under way is withheld.
bool ReleaseUnderWay(void *hostHandle) {
    if (!g_releasing.Blocks(hostHandle)) return false;
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true)) Log("menu mode: an evaluate of feature %p arrived during its release; withheld (NGX failure code, logged once)", hostHandle);
    return true;
}

// Ruling B, without the lock: true only when no pass may still use the model (at most 500 ms, as the drain).
bool ReleaseProven(const MenuPassProof &proof) {
    if (!proof.Needed()) return true;
    const double t0 = NowMs();
    const MenuPassWait result = MenuProvePass(proof, 500);
    if (MenuPassProven(result)) {
        if (result == MenuPassWait::Waited) Log("menu pipeline: the release waited %.1f ms for the last menu pass", NowMs() - t0);
        return true;
    }
    std::lock_guard lock(PipelineMutex());
    if (result == MenuPassWait::TimedOut) { // a live device (final review I2): the feature is kept, menu mode is not quarantined
        Log("menu mode: the watched feature is released while the last menu pass did not complete within 500 ms (a slow GPU); the "
            "feature is kept (leaked) rather than freed under it");
        return false;
    }
    Quarantine(P(), P().presents, result == MenuPassWait::Unfenced
                                      ? "the watched feature is released while a menu pass that was never fenced may still run"
                                      : "the watched feature is released after the device was removed, with a menu pass not proven finished");
    return false;
}
} // namespace

MenuHostEvaluate::MenuHostEvaluate(void *hostHandle, void *params, ID3D12GraphicsCommandList *hostList)
    : hostHandle_(hostHandle), params_(params), list_(hostList) {
    if (!hostHandle || !params) return;
    const bool on = MenuModeOn();
    // g_watching before the tombstone: the release sets the tombstone before it clears g_watching, so an evaluate that
    // sees g_watching cleared also sees the tombstone.
    const bool watching = g_watching.load();
    if (ReleaseUnderWay(hostHandle)) { withheld_ = true; return; }
    if (!on && !watching && !MenuFlowMayBeOn()) return;
    LastPass last;
    MenuPassProof proof;
    {
        std::lock_guard lock(PipelineMutex());
        auto &p = P();
        if (ReleaseUnderWay(hostHandle)) { withheld_ = true; return; } // the tombstone is set under this lock
        if (p.remembered && p.hostHandle != hostHandle) {
            if (on && !p.loggedOther) {
                p.loggedOther = true;
                Log("menu mode: a second feature %p evaluates; ignored while %p is watched (logged once)", hostHandle, p.hostHandle);
            }
            // Outside a run, an owed switch-back applies to every feature's evaluate (the motion source is the core's).
            if (!p.state.Active() && !MenuFlowBeforeHostEvaluate(hostHandle)) withheld_ = true;
            return;
        }
        hookWanted_ = on && p.remembered && list_ && HostQueue().UnobservedFor(kMenuSubmitHookPresents);
        if (on) {
            candidate_ = true;
            releases_ = MenuBook().Releases(); // releases clear the book under the lock too (MenuFeatureReleased)
            settingsEpoch_ = MenuSettingsEpoch(); // before the core reads the settings for this evaluate
        }
        if (p.remembered) {
            p.leases.Take(); // before the proof, until this object goes (after the core's evaluate returned)
            leased_ = true;
            resetTaken_ = AtHostEvaluate(p, &last);
            proof = ProofOf(p, false);
        }
        // Fix round 1 (Codex I1): no run is active now (AtHostEvaluate ended one). A switch-back the run's end could not
        // make is retried before the core runs this evaluate; while the core refuses it, the evaluate is withheld (NGX
        // failure code; the exit reset stays owed) and the next one retries.
        const bool vectors = MenuFlowBeforeHostEvaluate(hostHandle);
        if (p.noteGameEvaluate) {
            p.noteGameEvaluate = false;
            MenuEvent("game %llu %s", p.presents, vectors ? "vectors" : "withheld");
        }
        if (!vectors) { withheld_ = true; return; }
    }
    // Before featureCallMutex and the model call, without the pipeline's lock: the host's NR queue follows the last pass
    // on the GPU (C2), and the model runs only once the pass is proven complete on the CPU (ruling A), so a re-creation in
    // this evaluate never frees a model a pass still uses. Usually the pass completed long ago: no wait.
    // A pass already too slow once (held, expired: final review I2) is checked first and never waited for again: until it
    // completed, the evaluate is withheld at once, and the exit order below does not wait for it either.
    const bool heldSlow = proof.outstanding && !proof.wait && !proof.unfenced;
    if (heldSlow && !Proven(hostHandle, proof)) { withheld_ = true; return; }
    if (last.f2 && !OrderExit(hostHandle, list_, last)) { withheld_ = true; return; }
    if (!heldSlow && proof.Needed() && !Proven(hostHandle, proof)) { withheld_ = true; return; }
    if (resetTaken_) reset_ = std::make_unique<MenuExitReset>(params);
    if (candidate_) MenuSetEvaluatingHost(hostHandle);
}

MenuHostEvaluate::~MenuHostEvaluate() {
    if (candidate_) MenuSetEvaluatingHost(nullptr);
    // The exit reset this evaluate took did not reach a successful model call (withheld, the call failed, the core
    // carried the frame, or the shell forwarded it without the core): the next host evaluate owes it.
    if ((resetTaken_ && !resetRead_) || leased_) {
        std::lock_guard lock(PipelineMutex());
        auto &p = P();
        if (resetTaken_ && !resetRead_) p.state.RestoreResetOwed();
        if (leased_) p.leases.Give(); // HookEvaluate has left featureCallMutex and the core by now
    }
    if (hookWanted_) EnsureHostSubmitHook(list_); // once per session; this recording is submitted after it
}

void *MenuHostEvaluate::Params() const { return reset_ ? reset_->Params() : params_; }

void MenuHostEvaluate::Evaluated(void *currentModel, bool modelCalled, bool succeeded) {
    const bool ran = modelCalled && succeeded && currentModel;
    resetRead_ = ran;
    MenuParamBook &book = MenuBook();
    // A re-created model (or none) or a failed model call: the served snapshot names a model that must not run again.
    // Final review C1: on every evaluate of the bound feature, with the checkbox on or off (the book's leaf lock only; it
    // ignores other features), so a model re-created while the checkbox was off never leaves its snapshot behind.
    book.AfterHostEvaluate(hostHandle_, currentModel, modelCalled && !succeeded);
    if (!candidate_) return;
    MenuTag bound;
    {
        std::lock_guard lock(PipelineMutex());
        // A release since this evaluate began wins: neither the pipeline nor the book takes the feature up again.
        if (book.Releases() != releases_) return;
        auto &p = P();
        const bool watched = ran ? Remember(p, hostHandle_, params_, list_) : p.remembered && p.hostHandle == hostHandle_;
        if (!watched) return;
        HostQueue().ListRecorded(list_); // the watched feature's latest recording: the entry fence's host queue
        if (!ran) return;
        // The model the core evaluates now: a new pair (the first evaluate, a re-created model) starts a generation,
        // whose next evaluates MenuTracedEvaluate traces. The core's extra model passes run other handles, never bound.
        if (!book.BindIfNoRelease(hostHandle_, currentModel, releases_, &bound)) return;
    }
    // Build, guides and Publish lock themselves; a release from here on bumps the generation, so Publish drops it, and
    // this evaluate builds, publishes and withdraws only for `bound`.
    const char *problem = MenuSnapshotHostEvaluate(list_, params_, bound, settingsEpoch_);
    std::lock_guard lock(PipelineMutex());
    auto &p = P();
    if (p.remembered && p.hostHandle == hostHandle_) p.snapshotProblem = problem;
}

bool MenuFeatureReleased(void *hostHandle) {
    MenuPassProof proof;
    const double t0 = NowMs();
    {
        std::lock_guard lock(PipelineMutex()); // with the book's Clear: a host evaluate that began before binds nothing after it
        const double waited = NowMs() - t0; // a present under way (a menu pass, its core call) finishes first
        if (!ReleaseWatched(P(), hostHandle, &proof)) return true;
        if (Dumping() && waited >= 0.05) Log("menu mode: the release of feature %p waited %.2f ms for a present under way", hostHandle, waited);
    }
    return ReleaseProven(proof); // before HookRelease hands the feature to the core's release
}

void MenuFeatureReleaseDone(void *hostHandle) { g_releasing.Clear(hostHandle); }

} // namespace ofps::reshade
