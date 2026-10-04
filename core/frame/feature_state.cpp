#include "core/frame/feature_state.h"

#include "core/context.h"
#include "core/frame/model_grid.h"
#include "core/frame/warp_recorder.h"
#include "core/temporal/controller.h"
#include "core/api/ofps_settings_schema.h"

#include <memory>
#include <cstdio>
#include <cstring>
#include <utility>

namespace ofps::core {

void BuryReal(IOfpsModelHost *modelHost, void *realHandle, const ofps::core::gpu::GateSet &gate)
{
    if (realHandle == nullptr) return;
    ofps::core::gpu::Grave g;
    g.modelHandle = realHandle;
    g.modelHost = modelHost;
    g.gate = gate;
    Ctx().graveyard.Add(std::move(g), Ctx().evalCounter);
}

gpu::GateSet RetirementGate(FeatureState &st, const gpu::GateSet &extra)
{
    auto gate = gpu::SignalGate(st.realDevice, st.device);
    if (st.realDevice || st.device) Ctx().submission.PendingGate(st.realDevice, st.device, &gate);
    else Ctx().submission.PendingGateAll(&gate);
    // Device aliases can already be gone after RetireGpu; its ticket keeps their pending serials.
    gate.Append(st.asyncTicket);
    gate.Append(extra);
    if (st.async && st.async->fModel) {
        const auto &a = *st.async;
        gate.Add(a.fModel, a.recording ? a.jobId + 1 : a.jobId);
    }
    return gate;
}

bool BuryAsync(FeatureState &st, ofps::core::gpu::GateSet *gate)
{
    *gate = RetirementGate(st, *gate);
    if (!st.async) return false;
    st.async->Settle();
    st.asyncTicket = *gate;
    ofps::core::gpu::Grave g;
    g.gate = *gate;
    g.modelHost = st.modelHost;
    g.disposables.push_back(std::move(st.async));
    Ctx().graveyard.Add(std::move(g), Ctx().evalCounter);
    return !gate->Empty();
}

// Moves the feature's GPU objects into the graveyard without waiting.
void BuryGpu(FeatureState &st, const ofps::core::gpu::GateSet &gate)
{
    ofps::core::gpu::Grave g;
    g.gate = gate;
    g.modelHost = st.modelHost;
    if (st.spread) g.disposables.push_back(std::move(st.spread));
    if (st.passStaging) g.objects.push_back(st.passStaging);
    st.passStaging = nullptr;
    g.adapter12 = std::move(st.adapter);
    if (st.compute) g.disposables.push_back(std::move(st.compute));
    if (st.temporal) g.disposables.push_back(std::move(st.temporal));
    if (st.motionSmooth) g.disposables.push_back(std::move(st.motionSmooth));
    if (st.async) g.disposables.push_back(std::move(st.async));
    if (st.answer) g.objects.push_back(st.answer);
    if (st.frameSnapshot) g.objects.push_back(st.frameSnapshot);
    st.answer = st.frameSnapshot = nullptr;
    if (st.nrOutput) g.objects.push_back(st.nrOutput);
    if (st.unpackTarget) g.objects.push_back(st.unpackTarget);
    if (st.unpackBase) g.objects.push_back(st.unpackBase);
    if (st.rtvHeap) g.objects.push_back(st.rtvHeap);
    if (st.timingHeap) g.objects.push_back(st.timingHeap);
    if (st.timingReadback) g.objects.push_back(st.timingReadback);
    // 26.21: everything else the GPU writes into or reads from goes under the same gate; ReleaseGpu
    // would otherwise free them at once while the copies of the last recorded frame still run.
    if (st.motionReadback) g.objects.push_back(st.motionReadback);
    st.motionReadback = nullptr;
    st.motionReadbackPending = false;
    st.nrOutput = nullptr;
    st.unpackTarget = nullptr;
    st.unpackBase = nullptr;
    st.rtvHeap = nullptr;
    st.timingHeap = nullptr;
    st.timingReadback = nullptr;
    Ctx().graveyard.Add(std::move(g), Ctx().evalCounter);
    st.ReleaseGpu(); // what is left: device references and the tracked states
    st.warpPath = warp::PackPath::None;
    st.warpPathReason[0] = '\0';
}

// Preserve both host submissions and background work before clearing the device aliases.
void RetireGpu(FeatureState &st)
{
    if (AsyncVerbose()) Log(false, "Optimizer FPS NGX hook [async] RetireGpu begins (async %d)", st.async ? 1 : 0);
    auto gate = RetirementGate(st);
    BuryAsync(st, &gate);
    st.asyncTicket = gate;
    BuryGpu(st, gate);
}

// The background job's pass is being recorded or has not finished on the GPU (no wait: the fence's
// completed value). Retiring the job then would settle it, a CPU wait on the evaluate path.
static bool BackgroundPassBusy(const FeatureState &st)
{
    if (!st.async) return false;
    const AsyncJob &a = *st.async;
    return a.recording || (a.inflight && a.fModel && a.fModel->GetCompletedValue() < a.jobId);
}

// The optional textures a set on `path` gets in the current configuration (warp/texture_plan.h).
static warp::TexturePlan PlanFor(const FeatureState &st, warp::PackPath path, DXGI_FORMAT colorView, DXGI_FORMAT outputView)
{
    warp::TextureNeeds needs;
    needs.path = path;
    needs.temporal = Ctx().temporal.mode != 0;
    needs.warpBase = Ctx().temporal.warpBase;
    needs.transferReplacesBase = TransferReplacesBase(st.layout, path, colorView, outputView);
    needs.background = BackgroundCapable(st);
    needs.copyFallback = st.unpackCopySeen;
    return warp::PlanTextures(needs);
}

bool EnsureGpu(FeatureState &st, ID3D12GraphicsCommandList *cmd, ID3D12Resource *color, ID3D12Resource *output, DXGI_FORMAT colorView, DXGI_FORMAT outputView,
               UINT outputSubresource)
{
    // Everything is created through the command list's own device. When the list is ReShade's
    // proxy, that device is the proxy too, so descriptor heaps come back as the wrapped objects the
    // proxy list expects; heaps from the underlying real device make the proxy fault inside D3D12.
    // The host's resources answer with the real device; the adapter compares adapters, not objects.
    ID3D12Device *device = nullptr;
    if (FAILED(cmd->GetDevice(IID_PPV_ARGS(&device))) || device == nullptr) {
        SetReason("command list has no device");
        return false;
    }
    const D3D12_RESOURCE_DESC colorDesc = color->GetDesc();
    const D3D12_RESOURCE_DESC outputDesc = output->GetDesc();
    if (colorView == DXGI_FORMAT_UNKNOWN) colorView = TypedView(colorDesc.Format, false);
    if (outputView == DXGI_FORMAT_UNKNOWN) outputView = TypedView(outputDesc.Format, false);
    // The compute Unpack writes this output; one that needs the copy fallback makes the set keep its
    // intermediates from now on. The typed-store checks are ComputePath::Create's own requirement for
    // outputView. Our own codec answer (codec_frame.cpp) is created with UAV access and is not judged:
    // a rebuild from inside a spread stage or a background kick would bury the state they are using.
    if (output != st.answer &&
        warp::NeedsCopyFallback(warp::TargetUnpackSupport(outputDesc, outputView, outputView, outputSubresource, true, true)))
        st.unpackCopySeen = true;
    const auto requested = static_cast<warp::RequestedPath>(Ctx().values.v[OFPS_SET_DEBUG_WARP_PATH].i);
    const bool sameSet = (st.adapter || st.compute) && st.device == device && st.colorView == colorView &&
        st.outputView == outputView && st.requestedWarpPath == requested &&
        st.gpuNativeWidth == st.layout.nativeWidth && st.gpuNativeHeight == st.layout.nativeHeight &&
        st.gpuWorkWidth == st.layout.workWidth && st.gpuWorkHeight == st.layout.workHeight;
    bool samePlan = st.texturePlan == PlanFor(st, st.warpPath, colorView, outputView);
    // A change of the optional textures alone waits for the background pass in flight, re-checked every
    // evaluate: the rebuild retires the job, and settling a running pass would stall this evaluate. The
    // current set serves meanwhile. Every reader of an optional texture checks that it exists and takes
    // its existing fallback without it (wantBase/useBase need unpackBase; an Unpack needing a missing
    // copy intermediate fails before recording), so nothing missing is ever read.
    if (sameSet && !samePlan && BackgroundPassBusy(st)) {
        if (!st.texturePlanDeferred)
            Log(false, "Optimizer FPS NGX hook: texture set change deferred until the background pass completes");
        st.texturePlanDeferred = true;
        samePlan = true;
    }
    if (sameSet && samePlan) {
        // A compute list (the host's, or our background one) takes only the compute path; a pixel
        // path built on a direct list is rebuilt below when the host's list turns out to be COMPUTE.
        if (cmd->GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT ||
            (cmd->GetType() == D3D12_COMMAND_LIST_TYPE_COMPUTE && st.compute &&
             st.warpPath == warp::PackPath::Compute)) {
            device->Release();
            return true;
        }
        // Our own background list is COMPUTE too; only the host's list (hostListCompute, which also
        // keeps the background mode off) triggers the rebuild.
        if (cmd->GetType() != D3D12_COMMAND_LIST_TYPE_COMPUTE || !st.hostListCompute) {
            device->Release();
            SetReason("warp path cannot record on command list type %d", (int)cmd->GetType());
            return false;
        }
        if (st.async) { gpu::GateSet gate; BuryAsync(st, &gate); } // a job from the direct-list frames
        Log(false, "Optimizer FPS NGX hook: the host's list is COMPUTE now; rebuilding the warp on the compute path");
    }
    if (st.codecGridActive) {
        BuryWarpGpu(st);
        if (st.device) st.device->Release();
        if (st.realDevice) { st.realDevice->Release(); st.realDevice = nullptr; }
    } else RetireGpu(st);
    st.device = device; // keeps the reference taken above
    output->GetDevice(IID_PPV_ARGS(&st.realDevice));
    if (st.realDevice) {
        ID3D12Device *none = nullptr;
        if (Ctx().warpDevice.compare_exchange_strong(none, st.realDevice)) st.realDevice->AddRef();
    }

    if (!LoadShaders()) {
        SetReason("shaders missing");
        return false;
    }
    std::string pathReason;
    const auto decision = warp::ProbePack(device, requested, cmd->GetType(), colorView,
                                          st.async != nullptr,
                                          Ctx().shaders.WarpLoaded(), pathReason);
    if (decision.path == warp::PackPath::None) {
        SetReason("compute warp unavailable: %s", pathReason.c_str());
        return false;
    }
    if (cmd->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT &&
        !(cmd->GetType() == D3D12_COMMAND_LIST_TYPE_COMPUTE && decision.path == warp::PackPath::Compute)) {
        SetReason("warp path cannot record on command list type %d", (int)cmd->GetType());
        return false;
    }
    std::unique_ptr<warp::ComputePath> compute;
    if (decision.path == warp::PackPath::Compute) {
        const auto plan = PlanFor(st, warp::PackPath::Compute, colorView, outputView);
        compute = warp::ComputePath::Create(device, st.layout, colorView, outputView,
                                            plan.packSlots, plan.copyIntermediates, pathReason);
        if (!compute && requested == warp::RequestedPath::Compute) {
            SetReason("compute warp unavailable: %s", pathReason.c_str());
            RetireGpu(st);
            return false;
        }
        if (compute) pathReason = "compute path ready";
    }
    const bool useCompute = compute != nullptr;
    if (!useCompute && cmd->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT) {
        SetReason("pixel warp requires a DIRECT background list: %s", pathReason.c_str());
        return false;
    }
    const auto plan = PlanFor(st, useCompute ? warp::PackPath::Compute : warp::PackPath::Pixel, colorView, outputView);
    if (!useCompute) {
        const ofps::sdk::ShaderSet shaders = Ctx().shaders.Set();
        const ofps::sdk::D3D12TargetFormats packFormats{colorView, DXGI_FORMAT_R32_FLOAT,
            DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R16_FLOAT};
        const ofps::sdk::D3D12TargetFormats unpackFormats{outputView, DXGI_FORMAT_R32_FLOAT,
            DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R16_FLOAT};
        st.packSets.Reset(FeatureState::kPackSlots * 2, FeatureState::kPackSetRing);
        st.adapter = std::make_unique<ofps::sdk::D3D12Adapter>();
        // Packed textures for the pack slots alone; the unpack sets (kPackSlots + slot) are source sets
        // only (warp_recorder.cpp), so the set count stays that of the full slot-addressed range + ring.
        const auto status = st.adapter->Initialize(device, st.layout, packFormats, unpackFormats, shaders,
            plan.packSlots, false, FeatureState::kPackSlots * 2 + FeatureState::kPackSetRing);
        if (status != ofps::sdk::AdapterStatus::Ok) {
            SetReason("adapter init: %s (colour %d, output %d)", ofps::sdk::AdapterStatusString(status),
                (int)colorView, (int)outputView);
            st.adapter.reset(); return false;
        }
    }
    const auto nativeFlags = useCompute ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS :
        D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    if (!CreateTexture(device, st.layout.workWidth, st.layout.workHeight, outputView,
                       D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, &st.nrOutput) ||
        (plan.unpackTarget && !CreateTexture(device, st.layout.nativeWidth, st.layout.nativeHeight, outputView,
                                             nativeFlags, &st.unpackTarget)) ||
        (plan.unpackBase && !CreateTexture(device, st.layout.nativeWidth, st.layout.nativeHeight, outputView,
                                           nativeFlags, &st.unpackBase))) {
        SetReason("could not allocate the work output or the unpack target (format %d)", (int)outputView);
        st.ReleaseGpu(); return false;
    }
    if (!useCompute) {
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heapDesc.NumDescriptors = 2;
        if (FAILED(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&st.rtvHeap)))) {
            SetReason("could not create the RTV heap"); st.ReleaseGpu(); return false;
        }
        D3D12_RENDER_TARGET_VIEW_DESC rtv{};
        rtv.Format = outputView;
        rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        device->CreateRenderTargetView(st.unpackTarget, &rtv, st.rtvHeap->GetCPUDescriptorHandleForHeapStart());
        D3D12_CPU_DESCRIPTOR_HANDLE h = st.rtvHeap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        if (st.unpackBase) device->CreateRenderTargetView(st.unpackBase, &rtv, h); // read only when the base exists
    }
    st.unpackBaseState = D3D12_RESOURCE_STATE_COMMON;
    st.texturePlan = plan;
    st.texturePlanDeferred = false;
    st.gpuNativeWidth = st.layout.nativeWidth; st.gpuNativeHeight = st.layout.nativeHeight;
    st.gpuWorkWidth = st.layout.workWidth; st.gpuWorkHeight = st.layout.workHeight;
    st.colorView = colorView;
    st.outputView = outputView;
    st.compute = std::move(compute);
    st.warpPath = st.compute ? warp::PackPath::Compute : warp::PackPath::Pixel;
    st.requestedWarpPath = requested;
    std::snprintf(st.warpPathReason, sizeof(st.warpPathReason), "%s", pathReason.c_str());
    if (st.warpPath == warp::PackPath::Pixel) {
        std::snprintf(st.warpPathReason, sizeof(st.warpPathReason),
            "pixel path: %.110s; graphics state on the host's list is not restored", pathReason.c_str());
    }
    if (st.loggedWarpPath != st.warpPath ||
        std::strcmp(st.loggedWarpPathReason, st.warpPathReason) != 0) {
        Log(st.warpPath == warp::PackPath::Pixel, "Optimizer FPS NGX hook: %s", st.warpPathReason);
        st.loggedWarpPath = st.warpPath;
        std::snprintf(st.loggedWarpPathReason, sizeof(st.loggedWarpPathReason), "%s", st.warpPathReason);
    }
    std::size_t queuesTotal = 0;
    const std::size_t queuesOnDevice = ofps::core::gpu::CountQueues(st.realDevice, st.device, &queuesTotal);
    Log(false, "Optimizer FPS NGX hook: GPU path ready (colour view %d, output view %d, work %ux%u, list device %p, resource device %p, graphics queues on this device: %zu, queues registered: %zu)",
        (int) colorView, (int) outputView, st.layout.workWidth, st.layout.workHeight, (void *) device, (void *) st.realDevice,
        queuesOnDevice, queuesTotal);
    return true;
}

} // namespace ofps::core
