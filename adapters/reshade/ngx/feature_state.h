#pragma once

// Per-feature state of the feature-18 hook: everything one host handle owns (the real model, the
// GPU objects of the warp, the descriptor slots, the temporal machine and the background job).

#include "async_scheduler.h"
#include "hook_common.h"
#include "ngx_temporal.h"

#include "peripheral_warp/types_v2.h"

#include <cstdint>
#include <memory>

namespace pwhook {

// ---------------------------------------------------------------------------------------------
// Per-feature state. The key is the handle the host holds; the model may be re-created behind it.
// ---------------------------------------------------------------------------------------------
struct Tracked {
    ID3D12Resource *resource = nullptr;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
};

struct FeatureState {
    void *realHandle = nullptr;
    void *params = nullptr;
    std::uint32_t nativeWidth = 0, nativeHeight = 0;
    std::uint32_t createdWidth = 0, createdHeight = 0; // what the real feature was created with
    pw::ConfigV2 config{};
    pw::LayoutV2 layout{};
    bool warped = false;                                // the real feature expects the work frame
    bool disabled = false;                              // a resource failure: pass through until re-created
    // GPU objects
    ID3D12Device *device = nullptr;     // the command list's device (ReShade's proxy when present)
    ID3D12Device *realDevice = nullptr; // the device the host's resources answer with; keyed for GPU waits
    std::unique_ptr<pw::D3D12Adapter> adapter;
    DXGI_FORMAT colorView = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT outputView = DXGI_FORMAT_UNKNOWN;
    ID3D12Resource *nrOutput = nullptr;     // work-size, UAV, what the model writes
    ID3D12Resource *unpackTarget = nullptr; // native-size RTV, what Unpack writes, copied to the host output
    ID3D12Resource *unpackBase = nullptr;   // native-size RTV: the packed colour unpacked without the model (temporal base, stage 26.3)
    D3D12_RESOURCE_STATES unpackBaseState = D3D12_RESOURCE_STATE_COMMON;
    ID3D12DescriptorHeap *rtvHeap = nullptr; // [0] unpackTarget, [1] unpackBase
    // DebugTemporalReadback: centre texels copied to a readback buffer during an evaluate and
    // printed on a later one.
    ID3D12Resource *motionReadback = nullptr;
    bool motionReadbackPending = false;
    D3D12_RESOURCE_STATES nrOutputState = D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_STATES unpackState = D3D12_RESOURCE_STATE_COMMON;
    // Descriptor slots. A shader-visible descriptor or a constant slot must not be rewritten while
    // the GPU may still read it (the bridge queue runs several frames behind the CPU), so every slot
    // is written once for a given set of inputs and reused until those inputs change.
    static constexpr std::uint32_t kPackSlots = 5;          // slots 0..3 round-robin for the host's inputs, slot 4 for the background job's copies
    static constexpr std::uint32_t kBgPackSlot = kPackSlots - 1;
    D3D12_RESOURCE_STATES packedColorState[kPackSlots] = {};
    D3D12_RESOURCE_STATES packedGuideState[kPackSlots] = {};
    struct SlotKey {
        ID3D12Resource *color = nullptr, *depth = nullptr, *motion = nullptr;
        Subrect colorRect, depthRect, motionRect;
        float motionScaleX = 0.0f, motionScaleY = 0.0f;
        std::uint32_t depthInverted = 0;
        bool valid = false;
    };
    SlotKey packKeys[kPackSlots];
    bool unpackValid[kPackSlots] = {};
    pw::DepthConvention unpackDepthConvention[kPackSlots] = {};
    std::uint32_t nextPackSlot = 0;
    // External source sets (SRVs of the host's inputs + input constants) are CPU-written, so a set the
    // GPU may still read (the host's queue runs several frames behind) must not be rewritten. Hosts
    // that rotate their input textures every frame (BG3: 3 colour/output pairs x 4 motion textures)
    // would defeat a per-slot cache, so every evaluate takes a fresh set from a ring far deeper than
    // the queue; the packed textures keep their slots (their reuse is ordered by the queue).
    // Sets 0..kPackSlots*2-1 are the slot-addressed ones (background pack, unpack), the ring follows.
    static constexpr std::uint32_t kPackSetRing = 32;
    std::uint32_t nextPackSet = 0;
    struct InputSignature {
        DXGI_FORMAT colorFormat = DXGI_FORMAT_UNKNOWN, depthFormat = DXGI_FORMAT_UNKNOWN, motionFormat = DXGI_FORMAT_UNKNOWN, outputFormat = DXGI_FORMAT_UNKNOWN;
        std::uint32_t motionW = 0, motionH = 0;
        Subrect colorRect, depthRect, motionRect;
        float motionScaleX = 0.0f, motionScaleY = 0.0f;
        std::uint32_t depthInverted = 0;
        bool colorIsOutput = false;
        bool valid = false;
    } inputSignature;
    std::uint32_t frame = 0;
    // DebugTiming: timestamp pairs around the model call, resolved into a readback ring; a slot is
    // read only when it is a full ring older than the current evaluate (nothing in flight is touched).
    static constexpr std::uint32_t kTimingSlots = 32;
    ID3D12QueryHeap *timingHeap = nullptr;
    ID3D12Resource *timingReadback = nullptr;
    UINT64 timingFrequency = 0;
    std::uint64_t timingEval[kTimingSlots] = {};
    double timingSumFull = 0.0;
    std::uint32_t timingCountFull = 0;
    std::uint32_t timingCurrent = kTimingSlots; // slot begun by TimingBegin, consumed by TimingEnd
    // Temporal modes: the machine (residual, depth snapshot, accumulated motion), the number of
    // interpolated frames since the last full pass, and a per-feature switch-off with its reason.
    std::unique_ptr<pwtemporal::Machine> temporal;
    std::unique_ptr<AsyncJob> async; // background mode (3)
    // 26.21: gate of the last buried background job (its pass may still run the model). It is NOT a
    // GPU object of the feature: ReleaseGpu must not drop it, because RecreateReal / HookRelease run
    // after RetireGpu and still need it to gate the model's own release.
    pwngx::GateSet asyncTicket;
    std::uint32_t sinceFull = 0;
    bool temporalDisabled = false;
    bool temporalLogged = false;
    void ReleaseGpu()
    {
        adapter.reset();
        async.reset();
        temporal.reset();
        if (timingHeap) { timingHeap->Release(); timingHeap = nullptr; }
        if (timingReadback) { timingReadback->Release(); timingReadback = nullptr; }
        for (auto &e : timingEval) e = 0;
        timingCurrent = kTimingSlots;
        if (nrOutput) { nrOutput->Release(); nrOutput = nullptr; }
        if (unpackTarget) { unpackTarget->Release(); unpackTarget = nullptr; }
        if (unpackBase) { unpackBase->Release(); unpackBase = nullptr; }
        unpackBaseState = D3D12_RESOURCE_STATE_COMMON;
        if (rtvHeap) { rtvHeap->Release(); rtvHeap = nullptr; }
        if (motionReadback) { motionReadback->Release(); motionReadback = nullptr; }
        motionReadbackPending = false;
        if (device) { device->Release(); device = nullptr; }
        if (realDevice) { realDevice->Release(); realDevice = nullptr; }
        colorView = outputView = DXGI_FORMAT_UNKNOWN;
        for (auto &s : packedColorState) s = D3D12_RESOURCE_STATE_COMMON;
        for (auto &s : packedGuideState) s = D3D12_RESOURCE_STATE_COMMON;
        for (auto &k : packKeys) k = SlotKey{};
        for (auto &u : unpackValid) u = false;
        nextPackSlot = 0;
        nextPackSet = 0;
        inputSignature = InputSignature{};
        nrOutputState = unpackState = D3D12_RESOURCE_STATE_COMMON;
    }
};

// Builds (or rebuilds) the feature's GPU objects for the host's colour/output formats.
bool EnsureGpu(FeatureState &st, ID3D12GraphicsCommandList *cmd, ID3D12Resource *color, ID3D12Resource *output);
// The graveyard rules: nothing the GPU may still read is released before its gate has passed.
void BuryReal(void *realHandle, const pwngx::GateSet &gate = pwngx::GateSet{});
bool BuryAsync(FeatureState &st, pwngx::GateSet *gate);
void BuryGpu(FeatureState &st, const pwngx::GateSet &gate = pwngx::GateSet{});
void RetireGpu(FeatureState &st);

} // namespace pwhook
