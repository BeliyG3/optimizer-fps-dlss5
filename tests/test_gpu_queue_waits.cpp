// The core's registered queues (core/gpu/queues.h) on WARP: a queue retained for deferred work outlives its
// registration, the optical flow's GPU ordering waits only on queues of the same device identity, and succeeds
// only when every one of them accepted its Wait, and a proxy of the registered device is served by its queues.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "core/gpu/queues.h"
#include "test_menu_host_queue_fakes.h"
#include "test_proxy_device.h"
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
// renodx 8.5 / SF: the evaluate gets ReShade's proxy list; its device and the resources' are the proxy, while ReShade
// registered the queue under the native device. The registry serves the proxy with the one registered device on its
// adapter (the background mode's queue count, signal, wait and timestamp lookups, and the retirement gate); with two
// registered devices on that adapter it does not guess.
void TestProxyAlias(ID3D12Device *warp)
{
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue, second;
    ComPtr<ID3D12Fence> signalled, waited, probe;
    Check(SUCCEEDED(warp->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))) && SUCCEEDED(warp->CreateCommandQueue(&qd, IID_PPV_ARGS(&second))) &&
              SUCCEEDED(warp->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&signalled))) &&
              SUCCEEDED(warp->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&waited))) &&
              SUCCEEDED(warp->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&probe))),
          "proxy alias: queues and fences");
    proxytest::ProxyDevice proxy(warp), otherProxy(warp);
    Check(RegisterQueue(warp, queue.Get()), "proxy alias: the queue is registered under the native device");
    std::size_t total = 0;
    Check(CountQueues(&proxy, &proxy, &total) == 1 && total == 1, "proxy alias: the proxy counts the native device's graphics queue");
    Check(RegisteredAlias(&proxy, &proxy) == warp && RegisteredAlias(warp, &proxy) == nullptr,
          "proxy alias: the native device stands for the proxy only when no queue is keyed on either");
    Check(SignalRegisteredQueues(&proxy, &proxy, signalled.Get(), 1) && WaitFence(signalled.Get(), 1),
          "proxy alias: the background signal reaches the native device's queue");
    Check(WaitRegisteredQueues(&proxy, &proxy, waited.Get(), 1), "proxy alias: the background wait reaches it too");
    Check(SUCCEEDED(waited->Signal(1)) && SUCCEEDED(queue->Signal(probe.Get(), 1)) && WaitFence(probe.Get(), 1),
          "proxy alias: and the queue runs once the waited fence passed");
    UINT64 frequency = 0;
    Check(TimestampFrequency(&proxy, &proxy, &frequency) && frequency != 0, "proxy alias: timestamp frequency");
    {
        GateSet gate = SignalGate(&proxy, &proxy);
        Check(gate.Size() == 1, "proxy alias: the retirement gate covers the native device's queue");
        gate.Wait(3000);
        Check(gate.Completed(), "proxy alias: and completes");
    }
    // A second registered identity on the adapter: the proxy is not attributed to either.
    Check(RegisterQueue(&otherProxy, second.Get()), "proxy alias: a second device identity registered on the adapter");
    Check(CountQueues(&proxy, &proxy, &total) == 0 && total == 2 && RegisteredAlias(&proxy, nullptr) == nullptr,
          "proxy alias: two registered devices on the adapter give no alias");
    Check(CountQueues(warp, nullptr, &total) == 1, "proxy alias: an exact identity still matches its own queue");
    UnregisterQueue(second.Get());
    UnregisterQueue(queue.Get());
    Check(CountQueues(&proxy, &proxy, &total) == 0 && total == 0, "proxy alias: nothing registered, nothing matched");
}
} // namespace

int TestQueueWaits(ID3D12Device *warp, IDXGIFactory4 *factory)
{
    failures = 0;
    TestQueueLifetime(warp);
    TestWaitSameAdapter(warp, factory);
    TestWaitEveryQueue(warp);
    TestProxyAlias(warp);
    return failures;
}
