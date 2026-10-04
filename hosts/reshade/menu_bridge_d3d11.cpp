#include "hosts/reshade/menu_bridge_d3d11.h"
#include "hosts/reshade/menu_guides.h"
#include "hosts/reshade/menu_pipeline_state.h"
#include <dxgi.h>
#include <cstdio>

// The D3D11 <-> D3D12 bridge of the menu pipeline (spike 0c): shared textures and one shared fence.
namespace ofps::reshade {
using namespace menu_detail;

namespace {
bool Shareable(DXGI_FORMAT format) {
    return format == DXGI_FORMAT_R8G8B8A8_UNORM || format == DXGI_FORMAT_B8G8R8A8_UNORM ||
           format == DXGI_FORMAT_R16G16B16A16_FLOAT || format == DXGI_FORMAT_R10G10B10A2_UNORM;
}
} // namespace

bool MenuBridgeD3D11::Fail(const char *reason, HRESULT hr) {
    if (FAILED(hr)) std::snprintf(reason_, sizeof(reason_), "%s 0x%08X", reason, unsigned(hr));
    else std::snprintf(reason_, sizeof(reason_), "%s", reason);
    return false;
}

bool MenuBridgeD3D11::OpenDevice(ID3D11Device *d11, ID3D12Device *proxy) {
    if (device_) {
        if (d11 != d11raw_) return Fail("a second D3D11 device");
        return proxy == proxy_ || Fail("a second D3D12 device");
    }
    if (FAILED(d11->QueryInterface(IID_PPV_ARGS(&d11_)))) return Fail("no ID3D11Device5");
    ComPtr<ID3D11DeviceContext> immediate;
    d11->GetImmediateContext(&immediate);
    if (!immediate || FAILED(immediate.As(&ctx4_))) return Fail("no ID3D11DeviceContext4");
    ctxRaw_ = immediate.Get(); // the device keeps it alive
    ComPtr<IDXGIDevice> dxgi; ComPtr<IDXGIAdapter> adapter; DXGI_ADAPTER_DESC desc{};
    if (FAILED(d11->QueryInterface(IID_PPV_ARGS(&dxgi))) || FAILED(dxgi->GetAdapter(&adapter)) || FAILED(adapter->GetDesc(&desc)))
        return Fail("the D3D11 adapter is unknown");
    const LUID b = proxy->GetAdapterLuid();
    if (desc.AdapterLuid.LowPart != b.LowPart || desc.AdapterLuid.HighPart != b.HighPart) return Fail("the D3D11 and D3D12 adapter LUIDs differ");
    D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    // ReShade reports a queue of its wrapped device through init_command_queue inside this call (fail closed: a queue
    // another thread creates meanwhile counts too).
    const unsigned inits = g_reshadeQueueInits.load();
    HRESULT hr = proxy->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue_));
    if (FAILED(hr)) return Fail("private queue", hr);
    queueReported_ = g_reshadeQueueInits.load() != inits;
    timed_ = false; // a new queue: its GPU timestamps are built again
    for (auto &slot : ring_) {
        if (FAILED(hr = proxy->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&slot.allocator))) ||
            FAILED(hr = proxy->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, slot.allocator.Get(), nullptr, IID_PPV_ARGS(&slot.list))) ||
            FAILED(hr = slot.list->Close())) return Fail("private list", hr);
    }
    if (FAILED(hr = proxy->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence12_)))) return Fail("shared fence", hr);
    HANDLE handle = nullptr;
    if (FAILED(hr = proxy->CreateSharedHandle(fence12_.Get(), nullptr, GENERIC_ALL, nullptr, &handle))) return Fail("fence CreateSharedHandle", hr);
    hr = d11_->OpenSharedFence(handle, IID_PPV_ARGS(&fence11_));
    CloseHandle(handle);
    if (FAILED(hr)) return Fail("OpenSharedFence", hr);
    proxy_ = proxy; d11raw_ = d11;
    device_ = true;
    Log("menu bridge: D3D11 device %p (ID3D11Device5, ID3D11DeviceContext4) and D3D12 proxy %p on adapter LUID %08X:%08X; private DIRECT queue %p, "
        "shared fence %p (D3D11 %p), ring of %u", (void *) d11, (void *) proxy, unsigned(b.HighPart), unsigned(b.LowPart), (void *) queue_.Get(),
        (void *) fence12_.Get(), (void *) fence11_.Get(), kRing);
    return true;
}

bool MenuBridgeD3D11::Shared(const D3D12_RESOURCE_DESC &desc, ComPtr<ID3D12Resource> &d12, ComPtr<ID3D11Texture2D> &d11) {
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    HRESULT hr = proxy_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&d12));
    if (FAILED(hr)) return Fail("shared texture", hr);
    HANDLE handle = nullptr;
    if (FAILED(hr = proxy_->CreateSharedHandle(d12.Get(), nullptr, GENERIC_ALL, nullptr, &handle))) return Fail("texture CreateSharedHandle", hr);
    hr = d11_->OpenSharedResource1(handle, IID_PPV_ARGS(&d11));
    CloseHandle(handle);
    return SUCCEEDED(hr) || Fail("OpenSharedResource1", hr);
}

bool MenuBridgeD3D11::OpenPair(UINT width, UINT height, DXGI_FORMAT format) {
    if (!Shareable(format)) { char text[64]; std::snprintf(text, sizeof(text), "back buffer format %d is not shareable", int(format)); return Fail(text); }
    D3D12_RESOURCE_DESC td{}; td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; td.Width = width; td.Height = height;
    td.DepthOrArraySize = 1; td.MipLevels = 1; td.Format = format; td.SampleDesc.Count = 1;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    if (!Shared(td, in12_, in11_) || !Shared(td, out12_, out11_)) return false;
    // D3D11 refuses (E_INVALIDARG) a shared D3D12 texture with ALLOW_UNORDERED_ACCESS: the model pass, which writes its
    // output through a typed UAV, writes into a private texture that the private list then copies into `out`.
    if (MenuPassRunsModel()) {
        D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        const HRESULT hr = proxy_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&work12_));
        if (FAILED(hr)) return Fail("model output texture", hr);
    }
    width_ = width; height_ = height; format_ = format; lastUse_ = value_;
    Log("menu bridge: shared in/out %ux%u format %d (D3D12 %p/%p, D3D11 %p/%p)%s", width, height, int(format), (void *) in12_.Get(), (void *) out12_.Get(),
        (void *) in11_.Get(), (void *) out11_.Get(), work12_ ? ", the pass writes a private UAV texture copied into out" : "");
    return true;
}

bool MenuBridgeD3D11::Open(ID3D11Device *d11, ID3D12Device *d12Proxy, UINT width, UINT height, DXGI_FORMAT format) {
    reason_[0] = 0;
    if (poisoned_) return Fail(quarantined_ ? "quarantined (the last pass could not be proven finished, or the device was removed)"
                                     : "a fence operation failed (warm-up or pass)");
    if (!OpenDevice(d11, d12Proxy)) return false;
    in12_.Reset(); out12_.Reset(); in11_.Reset(); out11_.Reset(); work12_.Reset();
    open_ = OpenPair(width, height, format);
    if (!open_) { in12_.Reset(); out12_.Reset(); in11_.Reset(); out11_.Reset(); work12_.Reset(); } // never used by the GPU
    return open_;
}

MenuBridgeD3D11::Slot *MenuBridgeD3D11::Peek() {
    Slot &slot = ring_[next_ % kRing];
    return Passed(slot.value) ? &slot : nullptr;
}

bool MenuBridgeD3D11::Signal12(UINT64 value) {
    if (FAILED(queue_->Signal(fence12_.Get(), value))) { poisoned_ = true; return false; }
    value_ = value;
    return true;
}

bool MenuBridgeD3D11::Setup(const std::function<bool(ID3D12GraphicsCommandList *, Objects &, Objects &)> &record) {
    Slot *slot = Peek();
    if (!slot || !Begin(slot->allocator.Get(), slot->list.Get())) return false;
    Objects retire, keep;
    const bool recorded = record(slot->list.Get(), retire, keep);
    if (FAILED(slot->list->Close()) || !recorded) return false;
    // After the last value either side was told to reach (its signal was submitted): the fence never goes back.
    // A failed Wait or Signal here poisons the bridge: the pipeline stops menu mode for the session (fix round 1).
    if (value_ > 0 && FAILED(queue_->Wait(fence12_.Get(), value_))) { poisoned_ = true; return Fail("private queue warm-up wait"); }
    ID3D12CommandList *lists[] = {slot->list.Get()};
    queue_->ExecuteCommandLists(1, lists);
    ++next_;
    if (!Signal12(value_ + 1)) { // executed, never fenced: what it uses is kept for good (poisoned)
        slot->value = ~0ull;
        for (auto *objects : {&retire, &keep}) retained_.insert(retained_.end(), objects->begin(), objects->end());
        return Fail("private queue warm-up signal");
    }
    slot->value = value_;
    lastUse_ = value_; // the kept objects (the marker) are written by this submission
    for (auto &object : retire) graveyard_.push_back({object, value_});
    for (auto &object : keep) kept_.push_back(object);
    return true;
}

MenuBridgeResult MenuBridgeD3D11::Present(ID3D11DeviceContext *ctx, ID3D11Resource *backBuffer, MenuPass pass, bool haveLast) {
    recordMs = submitMs = submitParts[0] = submitParts[1] = submitParts[2] = 0;
    wroteBack_ = submitted_ = fenced_ = passWrote_ = false;
    if (!open_ || poisoned_) return MenuBridgeResult::Failed;
    if (ctx != ctxRaw_) { Fail("the present context is not the device's immediate context"); return MenuBridgeResult::Failed; }
    Slot *slot = Peek();
    if (!slot) return MenuBridgeResult::Skipped; // a full ring skips, never waits
    // The private list first: a pass that refuses leaves nothing enqueued on either side.
    double t = NowMs();
    if (!Begin(slot->allocator.Get(), slot->list.Get())) return MenuBridgeResult::Refused;
    ID3D12GraphicsCommandList *list = slot->list.Get();
    const unsigned timed = next_ % kRing;
    MenuGpuTimeBegin(list, timed);
    Barrier(list, in12_.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Barrier(list, out12_.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    // The pass contract: capture in COPY_SOURCE, output in COPY_DEST (work12_ rests there between passes).
    MenuFrame frame{in12_.Get(), work12_ ? work12_.Get() : out12_.Get(), list, width_, height_, format_};
    const bool ok = pass(frame);
    passWrote_ = frame.wrote;
    if (work12_) {
        Barrier(list, work12_.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->CopyResource(out12_.Get(), work12_.Get());
        Barrier(list, work12_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    }
    Barrier(list, out12_.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
    Barrier(list, in12_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    MenuGpuTimeEnd(list, timed);
    const bool closed = SUCCEEDED(list->Close());
    recordMs = NowMs() - t; t = NowMs();
    if (!ok || !closed) { MenuGuidesUnused(); return MenuBridgeResult::Refused; }
    // D3D11: the finished frame -> in, Signal(a), flushed so the signal is submitted before the D3D12 queue waits for it.
    ctx->CopyResource(in11_.Get(), backBuffer);
    const UINT64 a = value_ + 1, b = a + 1, c = b + 1;
    const auto failed = [&](const char *what, HRESULT hr, bool guidesUnused) {
        if (guidesUnused) MenuGuidesUnused();
        lastUse_ = value_; submitMs = NowMs() - t;
        Fail(what, hr);
        return MenuBridgeResult::Failed;
    };
    HRESULT hr = ctx4_->Signal(fence11_.Get(), a);
    if (FAILED(hr)) { poisoned_ = true; return failed("D3D11 signal of a", hr, true); } // nothing waits on a; the queued copy is never fenced: the pair is kept
    value_ = a;
    ctx->Flush();
    submitParts[0] = NowMs() - t;
    // D3D12: Wait(a), the pass, Signal(b); submitted before D3D11 is told to wait for b.
    if (FAILED(hr = queue_->Wait(fence12_.Get(), a))) return failed("D3D12 wait on a", hr, true); // the list never runs
    ID3D12CommandList *lists[] = {list};
    queue_->ExecuteCommandLists(1, lists);
    submitted_ = true; // the model ran on the menu frame, whatever follows (the exit reset is owed)
    lastList_ = list;
    ++next_;
    if (!Signal12(b)) { slot->value = ~0ull; return failed("D3D12 signal of b", E_FAIL, false); } // executed, never fenced: poisoned
    slot->value = b;
    passValue_ = b; fenced_ = true;
    submitParts[1] = NowMs() - t - submitParts[0];
    MenuGpuTimeSubmitted(timed, b);
    MenuGuidesUsed(fence12_.Get(), b);
    // D3D11: Wait(b), out -> back buffer, Signal(c) (the pair is free once c passed). A pass that left `out` alone is
    // shown only when `out` holds an earlier pass of this run.
    if (FAILED(hr = ctx4_->Wait(fence11_.Get(), b))) return failed("D3D11 wait on b", hr, false);
    if (passWrote_ || haveLast) {
        ctx->CopyResource(backBuffer, out11_.Get());
        wroteBack_ = true; // after Wait(b): correct, whatever follows
    }
    if (FAILED(hr = ctx4_->Signal(fence11_.Get(), c))) { poisoned_ = true; return failed("D3D11 signal of c", hr, false); }
    value_ = lastUse_ = c;
    submitMs = NowMs() - t;
    submitParts[2] = submitMs - submitParts[0] - submitParts[1];
    return MenuBridgeResult::Written;
}

MenuBridgeResult MenuBridgeD3D11::Reshow(ID3D11DeviceContext *ctx, ID3D11Resource *backBuffer, bool writeBack) {
    recordMs = submitMs = submitParts[0] = submitParts[1] = submitParts[2] = 0;
    wroteBack_ = submitted_ = fenced_ = false;
    if (!open_ || poisoned_) return MenuBridgeResult::Failed;
    if (ctx != ctxRaw_) { Fail("the present context is not the device's immediate context"); return MenuBridgeResult::Failed; }
    if (!writeBack) return MenuBridgeResult::Skipped;
    const double t = NowMs();
    // The context already holds Wait(b) of the pass that wrote `out` (its present enqueued it before its own write-back).
    ctx->CopyResource(backBuffer, out11_.Get());
    wroteBack_ = true;
    const HRESULT hr = ctx4_->Signal(fence11_.Get(), value_ + 1);
    if (FAILED(hr)) { poisoned_ = true; Fail("D3D11 signal of c (last output shown again)", hr); return MenuBridgeResult::Failed; }
    value_ = lastUse_ = value_ + 1; // the pair is read by this copy until then
    submitMs = NowMs() - t;
    return MenuBridgeResult::Written;
}

void MenuBridgeD3D11::Retire(Objects extra) {
    for (IUnknown *object : {static_cast<IUnknown *>(in12_.Get()), static_cast<IUnknown *>(out12_.Get()), static_cast<IUnknown *>(in11_.Get()),
                             static_cast<IUnknown *>(out11_.Get()), static_cast<IUnknown *>(work12_.Get())})
        if (object) graveyard_.push_back({ComPtr<IUnknown>(object), lastUse_});
    for (auto &object : kept_) graveyard_.push_back({object, lastUse_});
    for (auto &object : extra) graveyard_.push_back({object, lastUse_});
    kept_.clear();
    in12_.Reset(); out12_.Reset(); in11_.Reset(); out11_.Reset(); work12_.Reset();
    open_ = false;
}

void MenuBridgeD3D11::Poll() {
    if (poisoned_ || !fence12_ || Removed()) return; // a removed device proves nothing: kept
    const UINT64 done = fence12_->GetCompletedValue();
    if (done == UINT64_MAX) return; // the read that proves completion rejects a removal itself (fix round 2)
    std::erase_if(graveyard_, [&](const Retired &r) { return done >= r.value; });
}

void MenuBridgeD3D11::FlushSignals() {
    if (ctx4_) ctx4_->Flush();
}

bool MenuBridgeD3D11::Drain(DWORD ms) {
    if (!device_) return true;
    if (poisoned_) return false;
    if (Removed()) { Keep(); return false; } // nothing proves completion: never released (fix round 1)
    FlushSignals(); // the last Signal(c) may still sit in the D3D11 buffer: never wait for an unsubmitted value
    if (!Passed(value_) && !lateEvent_) {
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event) return false;
        if (FAILED(fence12_->SetEventOnCompletion(value_, event))) { CloseHandle(event); return false; }
        if (WaitForSingleObject(event, ms) != WAIT_OBJECT_0) { lateEvent_ = event; return false; } // the fence may still set it
        CloseHandle(event);
    }
    if (!Done()) {
        if (Removed()) Keep(); // removed while waiting
        return false;
    }
    Retire();
    Poll();
    return true;
}

MenuBridgeD3D11::~MenuBridgeD3D11() {
    if (lateEvent_) CloseHandle(lateEvent_);
}

} // namespace ofps::reshade
