#pragma once
// Stage 2 (spike 0c, product in Task 10): the menu pipeline for a D3D11 swap chain whose NR runs on a D3D12 device (a
// D3D11 -> D3D12 bridge). Per menu present, ordered by one shared fence only (no CPU waits):
//   D3D11 immediate context: back buffer -> in;                          Signal(a)
//   private DIRECT queue (the host list's D3D12 device, NGX's): Wait(a); pass: in -> out; Signal(b = a + 1)
//   D3D11 immediate context: Wait(b); out -> back buffer;                Signal(c = b + 1)
// in/out are D3D12 committed resources on a shared heap, opened in D3D11; on the D3D12 side they rest in
// COMMON whenever D3D11 owns them (the private list moves them to COPY_SOURCE/COPY_DEST and back).
// The pipeline (menu_pipeline_d3d11.cpp) registers the private queue with the core (C1), reports LastList() to it after
// each pass and keeps the last pass's (fence, PassValue()) as the proof record the host side waits on (menu_outstanding.h).
#include "hosts/reshade/menu_pipeline.h"
#include "hosts/reshade/menu_fence.h"
#include <d3d11_4.h>
#include <functional>
#include <vector>

namespace ofps::reshade {

enum class MenuBridgeResult { Written, Refused, Skipped, Failed };

class MenuBridgeD3D11 {
public:
    using Objects = std::vector<Microsoft::WRL::ComPtr<IUnknown>>;
    // Device-level objects on the first call (queue, ring, shared fence), then the size-bound pair in/out. False:
    // Reason() says which piece is missing; nothing that was created is used.
    bool Open(ID3D11Device *d11, ID3D12Device *d12Proxy, UINT width, UINT height, DXGI_FORMAT format);
    // One-time work on the private queue (the marker upload, the queue's warm-up): record fills `retire` with
    // objects the GPU may use until this submission completes and `keep` with objects retired with the pair.
    bool Setup(const std::function<bool(ID3D12GraphicsCommandList *list, Objects &retire, Objects &keep)> &record);
    // Records the pass first (a refusal enqueues nothing on either side), then the three submissions. A pass that left
    // its output alone (MenuFrame::wrote false) is written back only when `haveLast` (`out` holds an earlier pass of
    // this run); the Wait(b) and Signal(c) are enqueued either way.
    MenuBridgeResult Present(ID3D11DeviceContext *ctx, ID3D11Resource *backBuffer, MenuPass pass, bool haveLast);
    bool PassWrote() const { return passWrote_; } // the last Present's pass wrote its output
    // Stage 3 (no core call while the previous pass may run): D3D11 only, no private work. writeBack: out -> back buffer
    // (behind the previous present's Wait(b) in the context's order, so it shows that pass); then Signal(c). Written, or
    // Skipped when nothing was enqueued (writeBack false), or Failed (Signal(c): poisoned).
    MenuBridgeResult Reshow(ID3D11DeviceContext *ctx, ID3D11Resource *backBuffer, bool writeBack);
    // The pair (and what Setup kept) to the graveyard, released once the fence passed the last value that used it.
    void Retire(Objects extra = {});
    void Poll(); // releases retired objects whose value passed (checked, never waited on)
    // Submits the D3D11 signals still in the immediate context's buffer (Signal(c)); only on the thread that owns the
    // context (present, resize, swap chain destroy), before anything waits for the fence.
    void FlushSignals();
    bool Idle() const { return graveyard_.empty(); }
    bool Opened() const { return open_; }
    bool Poisoned() const { return poisoned_; }
    bool WroteBack() const { return wroteBack_; } // the last Present enqueued the write-back (also when it then Failed)
    bool Submitted() const { return submitted_; } // the last Present executed the private pass (also when it then Failed)
    bool Fenced() const { return fenced_; }       // ... and its Signal(b) succeeded: PassValue() is that pass
    UINT64 PassValue() const { return passValue_; } // b of the last pass the D3D12 queue signalled (0: none)
    ID3D12GraphicsCommandList *LastList() const { return lastList_; } // the list the last Submitted() Present executed
    // Quarantine (the last pass did not complete before the game's NR evaluate, or the device was removed): nothing is
    // released again.
    void Keep() { poisoned_ = quarantined_ = true; }
    // C1 (menu_pipeline_d3d11_gpu.cpp): the private queue is known to the core; keepRegistered: never unregistered
    // (the wrapped-device case, fix round 1).
    bool Registered() const { return registered_; }
    void MarkRegistered(bool registered, bool keepRegistered) { registered_ = registered; keepRegistered_ = keepRegistered; }
    bool KeepRegistered() const { return keepRegistered_; }
    // An init_command_queue event arrived while OpenDevice created the private queue (possibly ReShade reporting it: a
    // queue of ReShade's wrapped device). Only makes the registration permanent (fail closed); the pipeline reports
    // every pass itself regardless (fix round 2).
    bool QueueReported() const { return queueReported_; }
    bool Timed() const { return timed_; } // GPU timestamps were built for the current queue
    void MarkTimed() { timed_ = true; }
    // Swap chain destroyed (outside DllMain): flushes, waits at most `ms` for the last value; false = not passed (a
    // later Done() may still see it pass), poisoned or device removed (Keep(): never released).
    bool Drain(DWORD ms);
    bool Done() const { return !poisoned_ && (!fence12_ || Passed(value_)); } // everything passed
    // The device is removed (or cannot say): nothing is proven by the fence any more (fix round 1).
    bool Removed() const { return proxy_ && (proxy_->GetDeviceRemovedReason() != S_OK || (fence12_ && fence12_->GetCompletedValue() == UINT64_MAX)); }
    MenuBridgeD3D11() = default;
    MenuBridgeD3D11(const MenuBridgeD3D11 &) = delete;
    MenuBridgeD3D11 &operator=(const MenuBridgeD3D11 &) = delete;
    ~MenuBridgeD3D11(); // only once Done(): closes the event a timed-out Drain left with the fence
    const char *Reason() const { return reason_; }
    ID3D12Fence *Fence() const { return fence12_.Get(); }
    ID3D12CommandQueue *Queue() const { return queue_.Get(); }
    UINT64 Completed() const { return fence12_ ? MenuFenceProgress(fence12_.Get()) : 0; } // 0 on a removed device
    bool PassDone() const { return !fence12_ || Passed(passValue_); } // the last pass completed on a live device
    ID3D12Device *Device() const { return proxy_; }
    ID3D11Device *Device11() const { return d11_.Get(); }
    ID3D11Device *Device11Raw() const { return d11raw_; } // as the present event hands it over (null until opened)
    UINT Width() const { return width_; }
    UINT Height() const { return height_; }
    DXGI_FORMAT Format() const { return format_; }
    double recordMs = 0, submitMs = 0; // CPU of the last Present: the private list, the three submissions
    double submitParts[3] = {};        // of submitMs: D3D11 copy + Signal(a) + Flush, D3D12 Wait/Execute/Signal(b), D3D11 Wait/copy/Signal(c)

private:
    struct Slot { Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator; Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list; UINT64 value = 0; };
    struct Retired { Microsoft::WRL::ComPtr<IUnknown> object; UINT64 value = 0; };
    static constexpr unsigned kRing = 3;
    bool Fail(const char *reason, HRESULT hr = S_OK);
    // The fence reached `value` on a live device: the value read here is the proof, so it rejects a removed device's
    // UINT64_MAX itself (fix round 2), and the device must still answer S_OK (as MenuProvePass).
    bool Passed(UINT64 value) const {
        const UINT64 done = fence12_->GetCompletedValue();
        return done != UINT64_MAX && done >= value && !(proxy_ && proxy_->GetDeviceRemovedReason() != S_OK);
    }
    bool OpenDevice(ID3D11Device *d11, ID3D12Device *proxy);
    bool OpenPair(UINT width, UINT height, DXGI_FORMAT format);
    bool Shared(const D3D12_RESOURCE_DESC &desc, Microsoft::WRL::ComPtr<ID3D12Resource> &d12, Microsoft::WRL::ComPtr<ID3D11Texture2D> &d11);
    Slot *Peek();
    bool Signal12(UINT64 value);

    Microsoft::WRL::ComPtr<ID3D11Device5> d11_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext4> ctx4_;
    ID3D11Device *d11raw_ = nullptr;        // as the present event hands it over
    ID3D11DeviceContext *ctxRaw_ = nullptr; // the immediate context ctx4_ was queried from
    ID3D12Device *proxy_ = nullptr; // the host list's device (NGX's; native behind dlss5-dx11-bridge); not owned: valid while the host lives
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue_;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence12_;
    Microsoft::WRL::ComPtr<ID3D11Fence> fence11_;
    UINT64 value_ = 0; // the last value a Signal on either side was told to reach
    UINT64 passValue_ = 0;
    ID3D12GraphicsCommandList *lastList_ = nullptr; // owned by ring_
    Slot ring_[kRing];
    unsigned next_ = 0;
    Microsoft::WRL::ComPtr<ID3D12Resource> in12_, out12_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> in11_, out11_;
    Microsoft::WRL::ComPtr<ID3D12Resource> work12_; // model modes: the pass's UAV output (private), copied into out12_
    Objects kept_;
    Objects retained_; // used by a submission that was never fenced: never released (the bridge is poisoned)
    std::vector<Retired> graveyard_;
    HANDLE lateEvent_ = nullptr; // a timed-out Drain's event, still registered with the fence
    bool wroteBack_ = false, submitted_ = false, fenced_ = false, passWrote_ = false;
    UINT64 lastUse_ = 0; // the value after which nothing uses the current pair
    UINT width_ = 0, height_ = 0;
    DXGI_FORMAT format_ = DXGI_FORMAT_UNKNOWN;
    bool device_ = false, open_ = false, poisoned_ = false, quarantined_ = false;
    bool registered_ = false, keepRegistered_ = false, queueReported_ = false, timed_ = false;
    char reason_[160] = {};
};

// Bench diagnostics, only with DebugMenuBridgeCanary=1: the new messages of the D3D11 and D3D12 debug layers' info
// queues (when a layer is on), logged with `when`; counts per severity since the first call (menu_bridge_debug.cpp).
void MenuBridgeDebugMessages(ID3D11Device *d11, ID3D12Device *d12, const char *when);

} // namespace ofps::reshade
