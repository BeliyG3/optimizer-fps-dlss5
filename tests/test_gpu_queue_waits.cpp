// The core's registered queues (core/gpu/queues.h) on WARP: a queue retained for deferred work outlives its
// registration, and the optical flow's GPU ordering waits only on queues of the same device identity, and succeeds
// only when every one of them accepted its Wait.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "core/gpu/queues.h"
#include "test_menu_host_queue_fakes.h"
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <cstdint>
#include <iostream>

using Microsoft::WRL::ComPtr;
using namespace ofps::core::gpu;

namespace {
int failures = 0;
void Check(bool condition, const char *message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}
bool WaitFence(ID3D12Fence *fence, std::uint64_t value)
{
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (ev == nullptr)
        return false;
    const bool ok = WaitFenceValue(fence, value, ev, 3000);
    CloseHandle(ev);
    return ok;
}

void TestQueueLifetime(ID3D12Device *device)
{
    D3D12_COMMAND_QUEUE_DESC desc{};
    ComPtr<ID3D12CommandQueue> queue;
    Check(SUCCEEDED(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue))), "queue lifetime: create");
    Check(RegisterQueue(device, queue.Get()), "queue lifetime: register");
    auto *identity = queue.Get();
    ComPtr<ID3D12CommandQueue> retained;
    retained.Attach(RetainRegisteredQueue(identity));
    Check(retained != nullptr, "queue lifetime: retain during deferred processing");
    UnregisterQueue(identity);
    queue.Reset();
    Check(RetainRegisteredQueue(identity) == nullptr, "queue lifetime: stale ring entry is rejected");
    ComPtr<ID3D12Fence> fence;
    Check(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))), "queue lifetime: fence");
    Check(retained && SUCCEEDED(retained->Signal(fence.Get(), 1)) && WaitFence(fence.Get(), 1),
          "queue lifetime: retained async queue survives unregistration");
}

// Final review (Claude M5): the optical flow's GPU ordering (WaitEveryRegisteredQueue) falls back to queues keyed on
// another device only on the same adapter (ReShade keys its queues on the native device, the resources answer with its
// proxy: one adapter); a queue of another device never gets a Wait on a fence it cannot see.
void TestWaitSameAdapter(ID3D12Device *warp, IDXGIFactory4 *factory)
{
    ComPtr<IDXGIAdapter1> hardware;
    for (UINT i = 0; factory->EnumAdapters1(i, &hardware) != DXGI_ERROR_NOT_FOUND; ++i, hardware.Reset())
    {
        DXGI_ADAPTER_DESC1 desc{};
        if (SUCCEEDED(hardware->GetDesc1(&desc)) && (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0)
            break;
    }
    ComPtr<ID3D12Device> other;
    if (!hardware || FAILED(D3D12CreateDevice(hardware.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&other))))
    {
        std::cout << "same-adapter wait: no hardware adapter, skipped\n";
        return;
    }
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Fence> foreign, own, probe;
    Check(SUCCEEDED(warp->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))) &&
              SUCCEEDED(other->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&foreign))) &&
              SUCCEEDED(warp->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&own))) &&
              SUCCEEDED(warp->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&probe))),
          "same-adapter wait: queue and fences");
    Check(RegisterQueue(warp, queue.Get()), "same-adapter wait: the WARP queue is registered");
    Check(!WaitEveryRegisteredQueue(other.Get(), nullptr, foreign.Get(), 1),
          "same-adapter wait: no registered queue of another adapter waits for this device's fence");
    Check(SUCCEEDED(queue->Signal(probe.Get(), 1)) && WaitFence(probe.Get(), 1), "same-adapter wait: the WARP queue is not held");
    Check(WaitEveryRegisteredQueue(warp, nullptr, own.Get(), 1), "same-adapter wait: the device's own queue waits");
    Check(SUCCEEDED(own->Signal(1)) && SUCCEEDED(queue->Signal(probe.Get(), 2)) && WaitFence(probe.Get(), 2),
          "same-adapter wait: and runs once the fence passed");
    UnregisterQueue(queue.Get());
}
// Fix round 2 (Codex): the ordering succeeds only when every selected queue accepted its Wait. One refused Wait (the
// capture may be submitted on that queue) fails it, so the optical flow gets no session instead of racing.
void TestWaitEveryQueue(ID3D12Device *warp)
{
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> good, realBad;
    ComPtr<ID3D12Fence> gate, probe;
    Check(SUCCEEDED(warp->CreateCommandQueue(&qd, IID_PPV_ARGS(&good))) && SUCCEEDED(warp->CreateCommandQueue(&qd, IID_PPV_ARGS(&realBad))) &&
              SUCCEEDED(warp->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate))) &&
              SUCCEEDED(warp->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&probe))),
          "every queue: queues and fences");
    menutest::FailingQueue bad(realBad.Get(), false, true); // its Wait is refused
    Check(RegisterQueue(warp, good.Get()) && RegisterQueue(warp, &bad), "every queue: both queues are registered");
    Check(!WaitEveryRegisteredQueue(warp, nullptr, gate.Get(), 1) && bad.waits == 1,
          "every queue: one refused Wait fails the ordering, although the other queue accepted its own");
    UnregisterQueue(&bad);
    Check(WaitEveryRegisteredQueue(warp, nullptr, gate.Get(), 1), "every queue: with every Wait accepted it succeeds");
    Check(SUCCEEDED(gate->Signal(1)) && SUCCEEDED(good->Signal(probe.Get(), 1)) && WaitFence(probe.Get(), 1),
          "every queue: the accepted Waits pass once the fence did");
    UnregisterQueue(good.Get());
}
} // namespace

int TestQueueWaits(ID3D12Device *warp, IDXGIFactory4 *factory)
{
    failures = 0;
    TestQueueLifetime(warp);
    TestWaitSameAdapter(warp, factory);
    TestWaitEveryQueue(warp);
    return failures;
}
