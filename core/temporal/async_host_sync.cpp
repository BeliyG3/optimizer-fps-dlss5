#include "core/temporal/async_frame.h"

#include "core/context.h"
#include "core/gpu/queues.h"

#include <algorithm>
#include <cstdint>

// The background pass's hand-shake with the host's queue: copying the inputs on the host list,
// signalling that queue once the list is submitted, and capping how many host frames run ahead.
namespace ofps::core {

// Copies an input on the host list, restoring the host state and leaving its twin shader-readable.
void AsyncCopyInput(ID3D12GraphicsCommandList *cmd, ID3D12Resource *host, ID3D12Resource *bg, D3D12_RESOURCE_STATES &bgState,
                    D3D12_RESOURCE_STATES hostState, UINT hostSub)
{
    BarrierExternal(cmd, host, hostState, D3D12_RESOURCE_STATE_COPY_SOURCE, hostSub);
    Barrier(cmd, bg, bgState, D3D12_RESOURCE_STATE_COPY_DEST);
    if (hostSub != D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES) {
        D3D12_TEXTURE_COPY_LOCATION dstP{}, srcP{};
        dstP.pResource = bg; dstP.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dstP.SubresourceIndex = 0;
        srcP.pResource = host; srcP.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; srcP.SubresourceIndex = hostSub;
        cmd->CopyTextureRegion(&dstP, 0, 0, 0, &srcP, nullptr);
    } else {
        cmd->CopyResource(bg, host);
    }
    Barrier(cmd, bg, bgState, kModelInputState);
    BarrierExternal(cmd, host, D3D12_RESOURCE_STATE_COPY_SOURCE, hostState, hostSub);
}

void AsyncSignalPending(FeatureState &st)
{
    if (!st.async) return;
    AsyncJob &a = *st.async;
    // The submission event fires before the host's ExecuteCommandLists, so the signal is issued here,
    // one evaluate later, when the tagged list is certainly queued ahead of it. Without the event the
    // fallback signals every registered queue of the device.
    if (a.signalPending) {
        if (a.submitSeen && a.submitQueue) {
            a.submitQueue->Signal(a.fInputs, a.jobId);
        } else {
            ofps::core::gpu::SignalRegisteredQueues(st.realDevice, st.device, a.fInputs, a.jobId);
            if (!a.fallbackLogged) { a.fallbackLogged = true; Log(false, "Optimizer FPS NGX hook: background mode: host submission not observed, signalling the registered queues one frame later"); }
        }
        a.signalPending = false;
        a.submitSeen = false;
    }
}

void AsyncQueueCap(FeatureState &st)
{
    AsyncJob &a = *st.async;
    // Queue cap: the previous evaluate's list is already submitted (the host executes each frame's
    //    list before the next evaluate), so a signal issued now lands behind it; then wait on the CPU
    //    until at most maxQueue frames are unfinished.
    const int maxQueue = std::clamp(Ctx().temporal.maxQueue, 0, 8);
    if (maxQueue > 0 && a.fHost && a.hostEvent && Ctx().evalCounter > 1) {
        const std::uint64_t prev = Ctx().evalCounter - 1;
        if (prev > a.hostSignalled && ofps::core::gpu::SignalRegisteredQueues(st.realDevice, st.device, a.fHost, prev)) a.hostSignalled = prev;
        const std::uint64_t target = a.hostSignalled > static_cast<std::uint64_t>(maxQueue) ? a.hostSignalled - static_cast<std::uint64_t>(maxQueue) : 0;
        float waited = 0.0f;
        if (target > 0 && a.fHost->GetCompletedValue() < target) {
            LARGE_INTEGER f0, t0, t1;
            QueryPerformanceFrequency(&f0);
            QueryPerformanceCounter(&t0);
            ResetEvent(a.hostEvent);
            if (SUCCEEDED(a.fHost->SetEventOnCompletion(target, a.hostEvent))) WaitForSingleObject(a.hostEvent, 200);
            QueryPerformanceCounter(&t1);
            waited = static_cast<float>(t1.QuadPart - t0.QuadPart) * 1000.0f / static_cast<float>(f0.QuadPart);
            ++a.queueWaits;
        }
        a.queueWaitMs = a.queueWaitMs * 0.95f + waited * 0.05f;
    } else {
        a.queueWaitMs *= 0.95f;
    }
}

} // namespace ofps::core
