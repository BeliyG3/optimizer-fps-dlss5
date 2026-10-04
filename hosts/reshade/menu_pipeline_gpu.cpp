#include "hosts/reshade/menu_pipeline_state.h"
#include "hosts/reshade/menu_dump.h"
#include "hosts/reshade/menu_fence.h"
#include "hosts/reshade/menu_marker.h"
#include "hosts/reshade/shell_host.h"
#include <memory>
#include <utility>

// Menu mode's GPU objects: the private queue, fences and rings on the host list's device, the size-bound resources, the
// graveyard, and the drain on destroy_swapchain. Under the pipeline's lock, except WaitGpu (the drain's wait).
// Correction C1: the private queue is registered with the core (under the native device, see Build), so the core's
// retirement gates can be signalled on it too and wait for a menu pass still running there.
namespace ofps::reshade::menu_detail {

bool Build(Pipeline &p, ID3D12CommandQueue *presentQueue) {
    auto *g = p.gpu = new Gpu();
    if (FAILED(presentQueue->GetDevice(IID_PPV_ARGS(&g->native)))) return false;
    const LUID a = g->native->GetAdapterLuid(), b = p.proxy->GetAdapterLuid();
    if (a.LowPart != b.LowPart || a.HighPart != b.HighPart) { Log("menu pipeline: the present queue's adapter is not the feature's"); return false; }
    D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(p.proxy->CreateCommandQueue(&qd, IID_PPV_ARGS(&g->queue)))) return false;
    if (FAILED(p.proxy->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g->f1))) ||
        FAILED(p.proxy->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g->f2))) ||
        FAILED(p.proxy->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g->f3)))) return false;
    auto makeList = [](ID3D12Device *device, ComPtr<ID3D12CommandAllocator> &allocator, ComPtr<ID3D12GraphicsCommandList> &list) {
        return SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) &&
               SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list))) &&
               SUCCEEDED(list->Close());
    };
    for (auto &slot : g->nativeRing)
        if (!makeList(g->native.Get(), slot.captureAllocator, slot.captureList) || !makeList(g->native.Get(), slot.tailAllocator, slot.tailList)) return false;
    for (auto &slot : g->privateRing)
        if (!makeList(p.proxy, slot.allocator, slot.list)) return false;
    g->presentQueue = presentQueue;
    // C1: from here on the private queue can carry work; the core signals its retirement gates on it too.
    // Keyed on the NATIVE device (fix round 1, review I1), never on ReShade's proxy: the core keys the host's recordings
    // on the host list's device (the proxy) and Submission::Untrack CPU-cancels a device's unsubmitted recordings when
    // the last queue tracked under that device goes. Under the proxy key the private queue was that last queue, so its
    // unregister at a swap chain's teardown released resources of host lists not yet executed. Under the native key the
    // game's own queues (ReShade registers them natively) stay tracked, and no recording carries that key.
    // Coverage: in the ReShade host the feature's gates are keyed on the proxy (st.device and st.realDevice both answer
    // the proxy), so SignalGate/PendingGate do not name this queue. Menu mode does not rely on them (fix round 2): it
    // proves on the CPU that no pass may use the model before the core can free it, at every host evaluate of the
    // watched feature (a re-creation) and at its release (menu_pipeline_host.cpp, menu_outstanding.h).
    // RegisterQueue is void (ABI 1): it fails only on a fence error of that device, which the core logs ("could not create
    // a fence for queue ..."); the pipeline cannot detect it, so `registered` means "handed to the core".
    if (IOfpsCore *core = Core()) { core->RegisterQueue(g->native.Get(), g->queue.Get()); g->registered = true; }
    // Warm the private queue outside any menu: its first submission costs 1.5-2.7 ms of CPU.
    ID3D12CommandList *warm[] = {g->privateRing[0].list.Get()};
    g->queue->ExecuteCommandLists(1, warm);
    if (!Signal(g->queue.Get(), g->f2.Get(), g->v2)) { g->poisoned = true; return false; }
    g->privateRing[0].value2 = g->v2;
    if (!MenuGpuTimeBuild(p.proxy, g->queue.Get(), kRing)) Log("menu pipeline: no GPU timestamps on the private queue");
    Log("menu pipeline: DebugMenuPass=%d; private DIRECT queue %p on the proxy device %p (%s); present queue %p (native device %p); rings of %u",
        int(MenuPassSetting()), (void *) g->queue.Get(), (void *) p.proxy,
        g->registered ? "handed to the core under the native device" : "no core: not registered", (void *) presentQueue,
        (void *) g->native.Get(), kRing);
    return true;
}

void RetireResources(Gpu &g) {
    for (ComPtr<ID3D12Resource> *r : {std::addressof(g.capture), std::addressof(g.output), std::addressof(g.marker)})
        if (*r) { g.graveyard.push_back({ComPtr<IUnknown>(r->Get()), g.v2, g.v3}); r->Reset(); }
    for (auto &object : MenuModelRetire()) g.graveyard.push_back({object, g.v2, g.v3});
    MenuMarkerBind(nullptr);
    g.resources = false;
}

// Final review (Codex C1): values read before the removal check, so a removal after the read cannot make them lie; a
// removed device (UINT64_MAX or GetDeviceRemovedReason) poisons the objects: kept for the session, as on the bridge.
void ReleaseGraveyard(Gpu &g) {
    if (g.poisoned) return;
    const UINT64 done2 = g.f2->GetCompletedValue(), done3 = g.f3->GetCompletedValue();
    if (done2 == UINT64_MAX || done3 == UINT64_MAX || MenuFenceRemoved(g.f2.Get())) {
        g.poisoned = true;
        Log("menu pipeline: the device was removed; menu mode's GPU objects are kept for the session (nothing proves the GPU finished with them)");
        return;
    }
    std::erase_if(g.graveyard, [&](const Retired &r) { return done2 >= r.value2 && done3 >= r.value3; });
}

bool BuildResources(Pipeline &p, Gpu &g, const D3D12_RESOURCE_DESC &back, MenuColourSpace space, const MenuShape &shape) {
    const bool model = MenuPassRunsModel();
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td{}; td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; td.Width = back.Width; td.Height = back.Height;
    td.DepthOrArraySize = 1; td.MipLevels = 1; td.Format = back.Format; td.SampleDesc.Count = 1;
    if (FAILED(p.proxy->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g.capture))))
        return false;
    if (model) td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS; // the model pass converts into it
    if (FAILED(p.proxy->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_SOURCE, nullptr, IID_PPV_ARGS(&g.output))))
        return false;
    // ReShade wraps neither resources nor fences: what the proxy creates is native and the native queue can use it.
    // The capture answers GetDevice with the proxy, the present queue with the native device: only the LUIDs compare.
    ComPtr<ID3D12Device> answered;
    if (FAILED(g.capture->GetDevice(IID_PPV_ARGS(&answered)))) return false;
    const LUID a = answered->GetAdapterLuid(), n = g.native->GetAdapterLuid();
    const bool same = a.LowPart == n.LowPart && a.HighPart == n.HighPart;
    Log("menu pipeline: capture/output %llux%u format %d; the capture answers GetDevice with %p, LUID %s the present queue's device %p",
        back.Width, back.Height, int(back.Format), (void *) answered.Get(), same ? "equals" : "DIFFERS FROM", (void *) g.native.Get());
    if (!same) return false;
    if (MenuPassSetting() == MenuPassKind::Marker) {
        NativeSlot *slot = PeekNative(g);
        ComPtr<ID3D12Resource> upload;
        if (!slot || !Begin(slot->captureAllocator.Get(), slot->captureList.Get())) return false;
        const bool made = MenuMarkerCreate(p.proxy, back.Format, slot->captureList.Get(), &g.marker, &upload);
        if (FAILED(slot->captureList->Close()) || !made) { Log("menu pipeline: marker texture unavailable (format %d)", int(back.Format)); return false; }
        ID3D12CommandList *lists[] = {slot->captureList.Get()};
        g.presentQueue->ExecuteCommandLists(1, lists);
        const bool signalled = SignalNative(g, *slot);
        g.graveyard.push_back({ComPtr<IUnknown>(upload.Get()), 0, g.v3}); // a failed signal poisons: never released
        if (!signalled) return false;
        MenuMarkerBind(g.marker.Get());
    }
    if (model && !MenuModelBuild(p.proxy, back, shape)) { Log("menu pipeline: model pass resources unavailable"); return false; }
    g.dumps = Dumping() && MenuDumpPrepare(p.proxy, back);
    g.width = UINT(back.Width); g.height = back.Height; g.format = back.Format; g.space = space;
    g.modelIn = shape.colour.Format; g.modelOut = shape.output.Format;
    g.resources = true;
    return true;
}

// Fix round 3: the pending pass stays provable after g is detached (and leaked on a timeout): the last successfully
// fenced pass and any recovery fence live in p.outstanding (menu_outstanding.h), which holds its own fence references
// and is never cleared here, so a later evaluate or release of the watched feature still needs the proof.
Gpu *DetachGpu(Pipeline &p) {
    Gpu *g = p.gpu;
    p.gpu = nullptr;
    return g;
}

// Without the pipeline's lock (final review M1): f2, then f3, each until the shared deadline. An event armed for a wait
// that timed out stays open (the fence may still set it).
bool WaitGpu(Gpu &g, double deadlineMs) {
    if (g.poisoned) return false;
    HANDLE event = nullptr;
    bool armed = false; // the event is registered with a fence that has not set it yet
    bool drained = true;
    for (auto [fence, value] : {std::pair{g.f2.Get(), g.v2}, std::pair{g.f3.Get(), g.v3}}) {
        if (MenuFenceRemoved(fence)) { drained = false; break; } // proves nothing: FinishGpu keeps everything
        if (MenuFencePassed(fence, value)) continue;
        const double left = deadlineMs - NowMs();
        if (!event) event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (left <= 0 || !event) { drained = false; break; }
        if (FAILED(fence->SetEventOnCompletion(value, event))) { drained = false; break; }
        armed = true;
        if (WaitForSingleObject(event, DWORD(left) + 1) != WAIT_OBJECT_0) { drained = false; break; }
        armed = false;
    }
    if (event && !armed) CloseHandle(event);
    return drained && MenuFencePassed(g.f2.Get(), g.v2) && MenuFencePassed(g.f3.Get(), g.v3);
}

void FinishGpu(Pipeline &p, Gpu *g, bool drained) {
    if (!drained) { // the GPU may still use the lists, fences and resources: leak them
        const bool removed = MenuFenceRemoved(g->f2.Get()) || MenuFenceRemoved(g->f3.Get());
        const char *why = g->poisoned ? "a fence was never signalled (or the device was removed earlier)"
                        : removed     ? "the device was removed" : "a fence did not pass within the 500 ms of the teardown";
        if (removed) g->poisoned = true;
        Log("menu pipeline: %s; the GPU objects are kept (leaked, the private queue stays registered with the core), menu mode off for the session", why);
        EndSession(p, p.presents, "the GPU did not finish menu work at a swap chain's teardown (see ReShade.log)");
        return;
    }
    // C1: the queue's last fences passed; the core forgets it before the queue goes.
    if (g->registered) {
        if (IOfpsCore *core = Core()) core->UnregisterQueue(g->queue.Get());
        g->registered = false;
    }
    MenuDumpPoll(MenuFenceProgress(g->f3.Get()));
    MenuMarkerBind(nullptr);
    MenuDumpRelease();
    MenuGpuTimeRelease();
    for (auto &object : MenuModelRetire()) object.Reset();
    MenuModelReleaseDevice(); // the root signature and PSO belong to this device
    delete g;
    Log("menu pipeline: drained (all fences passed) and released");
}

} // namespace ofps::reshade::menu_detail
