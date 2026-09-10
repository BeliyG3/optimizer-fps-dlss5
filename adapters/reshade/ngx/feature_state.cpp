#include "feature_state.h"

#include "hook_context.h"

#include <memory>
#include <utility>

namespace pwhook {

void BuryReal(void *realHandle, const pwngx::GateSet &gate)
{
    if (realHandle == nullptr) return;
    pwngx::Grave g;
    g.realHandle = realHandle;
    g.releaseFn = reinterpret_cast<pwngx::PFN_Release>(Ctx().realRelease);
    g.gate = gate;
    Ctx().graveyard.Add(std::move(g), Ctx().evalCounter);
}

// Moves the background job into the graveyard, gated on its model fence: nothing it (or the feature's
// GPU objects buried with the same gate) uses is released before the pass in flight is over. No CPU
// wait: the host thread cannot wait on a signal it still owes the background queue. The gate is the
// single model fence of the background queue (one queue, one fence); it is written into `gate` and
// remembered as the feature's retirement ticket. False when there was no job or it had no fence.
bool BuryAsync(FeatureState &st, pwngx::GateSet *gate)
{
    gate->Clear();
    if (!st.async) return false;
    AsyncJob &a = *st.async;
    a.Settle(); // hands over the owed input signal
    if (a.fModel) gate->Add(a.fModel, a.jobId);
    // Remembered for the model's own release (RecreateReal / HookRelease run after RetireGpu).
    if (!gate->Empty()) st.asyncTicket = *gate;
    pwngx::Grave g;
    g.gate = *gate;
    g.disposables.push_back(std::move(st.async));
    Ctx().graveyard.Add(std::move(g), Ctx().evalCounter);
    return !gate->Empty();
}

// Moves the feature's GPU objects into the graveyard without waiting.
void BuryGpu(FeatureState &st, const pwngx::GateSet &gate)
{
    pwngx::Grave g;
    g.gate = gate;
    g.adapter12 = std::move(st.adapter);
    if (st.temporal) g.disposables.push_back(std::move(st.temporal));
    if (st.async) g.disposables.push_back(std::move(st.async));
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
}

// Drops the feature's GPU objects: immediately after a successful GPU wait, otherwise into the
// graveyard. Either way the feature ends up without GPU objects (ReleaseGpu semantics).
void RetireGpu(FeatureState &st)
{
    if (AsyncVerbose()) Log(false, "Optimizer FPS NGX hook [async] RetireGpu begins (async %d)", st.async ? 1 : 0);
    // A background pass in flight uses the adapter's textures and the model: everything goes to the
    // graveyard gated on its fence instead of being waited for.
    pwngx::GateSet gate;
    if (BuryAsync(st, &gate)) {
        BuryGpu(st, gate);
        return;
    }
    const bool anything = st.adapter || st.nrOutput || st.unpackTarget || st.rtvHeap || st.timingHeap || st.temporal;
    if (!anything) { st.ReleaseGpu(); return; }
    {
        pwngx::GateSet queueGate = pwngx::SignalGate(st.realDevice, st.device);
        if (!queueGate.Empty()) { BuryGpu(st, queueGate); return; }
    }
    if (WaitForGpu(st.realDevice, st.device)) st.ReleaseGpu();
    else BuryGpu(st);
}

bool EnsureGpu(FeatureState &st, ID3D12GraphicsCommandList *cmd, ID3D12Resource *color, ID3D12Resource *output)
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
    (void) output;
    const D3D12_RESOURCE_DESC colorDesc = color->GetDesc();
    const D3D12_RESOURCE_DESC outputDesc = output->GetDesc();
    const DXGI_FORMAT colorView = TypedView(colorDesc.Format, false);
    const DXGI_FORMAT outputView = TypedView(outputDesc.Format, false);
    if (st.adapter && st.device == device && st.colorView == colorView && st.outputView == outputView) {
        device->Release();
        return true;
    }
    RetireGpu(st);
    st.device = device; // keeps the reference taken above
    output->GetDevice(IID_PPV_ARGS(&st.realDevice));

    if (!LoadShaders()) {
        SetReason("shaders missing");
        return false;
    }
    const pw::ShaderSet shaders = Ctx().shaders.Set();
    const pw::D3D12TargetFormats packFormats{colorView, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R16G16_FLOAT,
                                             DXGI_FORMAT_R16_FLOAT};
    const pw::D3D12TargetFormats unpackFormats{outputView, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R16G16_FLOAT,
                                               DXGI_FORMAT_R16_FLOAT};
    st.adapter = std::make_unique<pw::D3D12Adapter>();
    // Slots 0..kPackSlots-1 pack (native sources), the next kPackSlots unpack (work sources); no confidence.
    // Source sets: the slot-addressed ones plus the ring the host frames' pack sets come from.
    const pw::AdapterStatus status =
        st.adapter->Initialize(device, st.layout, packFormats, unpackFormats, shaders, FeatureState::kPackSlots * 2, false,
                               FeatureState::kPackSlots * 2 + FeatureState::kPackSetRing);
    if (status != pw::AdapterStatus::Ok) {
        SetReason("adapter init: %s (colour %d, output %d)", pw::AdapterStatusString(status), (int) colorView,
                  (int) outputView);
        st.adapter.reset();
        return false;
    }
    if (!CreateTexture(device, st.layout.workWidth, st.layout.workHeight, outputView,
                       D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, &st.nrOutput) ||
        !CreateTexture(device, st.layout.nativeWidth, st.layout.nativeHeight, outputView,
                       D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, &st.unpackTarget) ||
        !CreateTexture(device, st.layout.nativeWidth, st.layout.nativeHeight, outputView,
                       D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, &st.unpackBase)) {
        SetReason("could not allocate the work output or the unpack target (format %d)", (int) outputView);
        st.ReleaseGpu();
        return false;
    }
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heapDesc.NumDescriptors = 2;
    if (FAILED(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&st.rtvHeap)))) {
        SetReason("could not create the RTV heap");
        st.ReleaseGpu();
        return false;
    }
    D3D12_RENDER_TARGET_VIEW_DESC rtv{};
    rtv.Format = outputView;
    rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    device->CreateRenderTargetView(st.unpackTarget, &rtv, st.rtvHeap->GetCPUDescriptorHandleForHeapStart());
    {
        D3D12_CPU_DESCRIPTOR_HANDLE h = st.rtvHeap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        device->CreateRenderTargetView(st.unpackBase, &rtv, h);
    }
    st.unpackBaseState = D3D12_RESOURCE_STATE_COMMON;
    st.colorView = colorView;
    st.outputView = outputView;
    std::size_t queuesTotal = 0;
    const std::size_t queuesOnDevice = pwngx::CountQueues(st.realDevice, st.device, &queuesTotal);
    Log(false, "Optimizer FPS NGX hook: GPU path ready (colour view %d, output view %d, work %ux%u, list device %p, resource device %p, queues for GPU waits: %zu of %zu on this device)",
        (int) colorView, (int) outputView, st.layout.workWidth, st.layout.workHeight, (void *) device, (void *) st.realDevice,
        queuesOnDevice, queuesTotal);
    return true;
}

} // namespace pwhook
