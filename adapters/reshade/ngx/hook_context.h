#pragma once

// The interposer's process-wide state (stage 27.D1). Everything that used to be a file-scope `g_*`
// of ngx_hook.cpp lives in one HookContext, reached through Ctx(); the modules address it by name.
// The context is a function-local static, so no module depends on another one's initialization
// order.

#include "feature_state.h"
#include "hook_common.h"

#include "../ngx_hook.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace pwhook {

struct HookContext {
    // ---- the real entry points of nvngx_dlssnr.dll (every call goes through the forwarder)
    PFN_Create realCreate = nullptr;
    PFN_Evaluate realEvaluate = nullptr;
    PFN_Release realRelease = nullptr;

    pw_ngx::GetConfigFn getConfig = nullptr;
    std::wstring shaderDirectory;
    // 26.26: the overlay writes these from the present/UI thread while the evaluate thread reads them.
    // Each is independent of the others (no invariant spans two), so a relaxed atomic is enough; the
    // settings that must move together (TemporalSettings) still go under the mutex in SetTemporal.
    std::atomic<bool> showCenterOutline{false};
    std::atomic<bool> showWorkOutline{false};
    std::atomic<float> motionScaleAdjust{1.0f};
    std::atomic<bool> motionInvert{false};
    std::atomic<float> outputGain{1.0f};
    std::atomic<float> outputGamma{1.0f};
    pw_ngx::TemporalSettings temporal;
    // 26.26: the diagnostic switches, read once from the read-only [PeripheralWarp] Debug* keys at add-on
    // load (producer.cpp) and never written back. They used to be PW_NGX_* environment variables; no
    // environment variable is read any more. Set before the first evaluate, read without a lock after.
    pw_ngx::DiagnosticsConfig diag;

    std::atomic<bool> enabled{true};
    // 26.21: fired once, before the first warped evaluate is recorded, so the crash guard's marker is on
    // disk even if that evaluate never returns (the marker used to be written from the next present).
    std::atomic<pw_ngx::FirstWarpedFn> firstWarpedCallback{nullptr};
    std::atomic<bool> firstWarpedFired{false};
    std::atomic<bool> safeMode{false}; // 26.16: forward everything untouched (the previous session of this game died after our first warped frame)
    // 26.22: features of a second NR consumer are not ours at all. They used to get a FeatureState and a pass-through
    // model created through our forwarder; in Control that create crashed inside the model's own CreateFeature
    // (D3D12Core SetDescriptorHeaps on the caller's list) although the same call works without the hook. Now the
    // original entry points are called with the caller's arguments untouched and the handle is only remembered so
    // that its evaluates/releases pass straight through too.
    std::unordered_set<void *> foreign;
    std::uint32_t hookAttempts = 0;
    ULONGLONG hookFirstPoll = 0;
    bool hookDelayLogged = false; // DebugHookDelayMs delays the hook so a host creates its model first; logged once
    std::mutex mutex;
    pw_ngx::Status status;
    std::uint64_t evalCounter = 0; // every HookEvaluate, warped or not

    pwngx::Shaders shaders;
    std::unordered_map<void *, std::unique_ptr<FeatureState>> features;
    pwngx::Graveyard graveyard; // under the mutex
    std::atomic<bool> anyAsyncSignalPending{false}; // 26.7.4: the execute_command_list handler touches nothing unless a background kick awaits its submit
    pwngx::CrashInfo crash;
    // [PeripheralWarp] DebugLayer=1: how many debug-layer messages have been printed so far (first 200).
    std::uint64_t debugLayerPrinted = 0;
};

HookContext &Ctx();

// ---- calls into the snippet through the forwarder
int CallCreate(ID3D12GraphicsCommandList *cmd, int featureId, void *params, void **outHandle);
int CallEvaluate(ID3D12GraphicsCommandList *cmd, void *handle, void *params, void *callback);
int CallRelease(void *handle);

bool KeepBackbufferEnabled(); // DebugKeepBackbuffer
void NotifyFirstWarped();
bool WaitForGpu(ID3D12Device *device, ID3D12Device *proxyDevice = nullptr);
// 26.18: releases and re-creations no longer stall the CPU. A fence is signalled on the host's queues and the
// objects go to the graveyard gated on it; they are dropped on a later evaluate once the GPU has passed the
// signal. The CPU wait is only reached when no queue could be signalled.
void SetReason(const char *fmt, ...);
bool LoadShaders();
void DrainGraveyard(bool everything);

bool TimingEnabled(); // DebugTiming=1
// Experiment: DebugTemporalKeepOutput=1 leaves the host's Output untouched on interpolated frames
// (the reprojection is still recorded into the machine's target).
bool KeepOutputOnInterpolation();
float ResidualBlendWeight();
#define kResidualBlend ResidualBlendWeight()

} // namespace pwhook
