#include "core/gpu/queues.h"
#include "core/log.h"
#include <mutex>

namespace ofps::core::gpu {
namespace {

struct QueueEntry {
    ID3D12Device *device = nullptr;
    ID3D12CommandQueue *queue = nullptr;
    ID3D12Fence *fence = nullptr;
    HANDLE event = nullptr;
    UINT64 value = 0;
    bool graphics = false; // DIRECT; compute queues only join the releases' GPU waits
};
std::mutex g_queueMutex;
std::vector<QueueEntry> g_queues;
bool g_noQueueLogged = false;
bool g_queueRegistrationSuppressed = false;


} // namespace

void SetQueueRegistrationSuppressed(bool suppressed) { g_queueRegistrationSuppressed = suppressed; }

bool RegisterQueue(ID3D12Device *device, ID3D12CommandQueue *queue)
{
    if (device == nullptr || queue == nullptr || g_queueRegistrationSuppressed) return false;
    QueueEntry e;
    e.device = device;
    e.queue = queue;
    e.graphics = queue->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&e.fence)))) {
        Log(true, "Optimizer FPS NGX: could not create a fence for queue %p; GPU waits unavailable on it", (void *) queue);
        return false;
    }
    e.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (e.event == nullptr) {
        e.fence->Release();
        return false;
    }
    device->AddRef();
    queue->AddRef();
    {
        std::lock_guard<std::mutex> lock(g_queueMutex);
        g_queues.push_back(e);
    }
    Log(false, "Optimizer FPS NGX: D3D12 %s queue %p (device %p) registered for GPU waits",
        e.graphics ? "graphics" : "compute", (void *) queue, (void *) device);
    return true;
}

ID3D12CommandQueue *RetainRegisteredQueue(ID3D12CommandQueue *queue)
{
    std::lock_guard<std::mutex> lock(g_queueMutex);
    for (const QueueEntry &e : g_queues) if (e.queue == queue) { queue->AddRef(); return queue; }
    return nullptr;
}

void UnregisterQueue(ID3D12CommandQueue *queue)
{
    QueueEntry gone{};
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(g_queueMutex);
        for (std::size_t i = 0; i < g_queues.size(); ++i) {
            if (g_queues[i].queue != queue) continue;
            gone = g_queues[i];
            g_queues.erase(g_queues.begin() + static_cast<std::ptrdiff_t>(i));
            found = true;
            break;
        }
    }
    if (!found) return;
    CloseHandle(gone.event);
    gone.fence->Release();
    gone.queue->Release();
    gone.device->Release();
}

// 26.21: a CPU wait that survives a stale event registration. A wait that timed out leaves its
// SetEventOnCompletion armed; when that older value completes the auto-reset event is left signalled
// and the NEXT wait would return WAIT_OBJECT_0 at once, before its own value was reached. So the
// event is reset before it is armed and every wake re-checks the fence, waiting on until the deadline.
bool WaitFenceValue(ID3D12Fence *fence, UINT64 value, HANDLE event, DWORD milliseconds)
{
    if (fence == nullptr) return false;
    const auto passed = [&] { const UINT64 done = fence->GetCompletedValue(); return done != UINT64_MAX && done >= value; };
    if (fence->GetCompletedValue() == UINT64_MAX) return false;
    if (passed()) return true;
    if (event == nullptr) return false;
    ResetEvent(event);
    if (passed()) return true; // completed while the event was being reset
    if (FAILED(fence->SetEventOnCompletion(value, event))) return false;
    const ULONGLONG deadline = GetTickCount64() + milliseconds;
    for (;;) {
        if (passed()) return true;
        if (fence->GetCompletedValue() == UINT64_MAX) return false;
        const ULONGLONG now = GetTickCount64();
        if (now >= deadline) return false;
        const DWORD waited = WaitForSingleObject(event, static_cast<DWORD>(deadline - now));
        if (waited == WAIT_OBJECT_0) continue;  // may be a stale registration: the loop re-checks
        if (waited == WAIT_TIMEOUT) return passed();
        return false; // WAIT_FAILED / abandoned
    }
}

bool WaitForGpu(ID3D12Device *device, ID3D12Device *proxyDevice)
{
    if (device != nullptr && FAILED(device->GetDeviceRemovedReason())) return false;
    std::lock_guard<std::mutex> lock(g_queueMutex);
    if (g_queues.empty()) {
        if (!g_noQueueLogged) {
            g_noQueueLogged = true;
            Log(true, "Optimizer FPS NGX: no D3D12 queue registered; releases are deferred by %llu evaluates instead of a GPU wait",
                static_cast<unsigned long long>(kDeferredReleaseEvaluates));
        }
        return false;
    }
    auto onDevice = [&](const QueueEntry &e) {
        return (device != nullptr && e.device == device) || (proxyDevice != nullptr && e.device == proxyDevice);
    };
    bool matched = false;
    for (const QueueEntry &e : g_queues) matched |= onDevice(e);
    bool ok = true;
    for (QueueEntry &e : g_queues) {
        if (matched && !onDevice(e)) continue;
        const UINT64 value = ++e.value;
        if (FAILED(e.queue->Signal(e.fence, value))) { ok = false; continue; }
        if (!WaitFenceValue(e.fence, value, e.event, kGpuWaitMilliseconds)) ok = false;
    }
    if (!ok) Log(true, "Optimizer FPS NGX: GPU wait failed or timed out; the release is deferred");
    return ok;
}

bool SignalRegisteredQueues(ID3D12Device *device, ID3D12Device *proxyDevice, ID3D12Fence *fence, UINT64 value)
{
    std::lock_guard<std::mutex> lock(g_queueMutex);
    bool any = false;
    for (const QueueEntry &e : g_queues)
        if (e.graphics && (e.device == device || e.device == proxyDevice)) { any |= SUCCEEDED(e.queue->Signal(fence, value)); }
    return any;
}

void GateSet::Add(ID3D12Fence *fence, UINT64 value)
{
    if (fence == nullptr) return;
    for (auto &gate : gates_) if (gate.fence == fence) {
        if (value > gate.value) gate.value = value;
        return;
    }
    fence->AddRef();
    gates_.push_back(FenceGate{fence, value});
}

void GateSet::Append(const GateSet &other)
{
    const auto count = other.gates_.size();
    for (std::size_t i = 0; i < count; ++i) Add(other.gates_[i].fence, other.gates_[i].value);
}

void GateSet::Assign(const GateSet &other)
{
    gates_ = other.gates_;
    for (FenceGate &g : gates_) if (g.fence) g.fence->AddRef();
}

void GateSet::Clear()
{
    for (FenceGate &g : gates_) if (g.fence) g.fence->Release();
    gates_.clear();
}

bool GateSet::Completed() const
{
    for (const FenceGate &g : gates_)
        if (g.fence && (g.fence->GetCompletedValue() == UINT64_MAX || g.fence->GetCompletedValue() < g.value)) return false;
    return true;
}

void GateSet::Wait(DWORD milliseconds) const
{
    if (gates_.empty()) return;
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (ev == nullptr) return;
    for (const FenceGate &g : gates_)
        if (g.fence) WaitFenceValue(g.fence, g.value, ev, milliseconds);
    CloseHandle(ev);
}

GateSet SignalGate(ID3D12Device *device, ID3D12Device *proxyDevice)
{
    GateSet set;
    if (device == nullptr) return set;
    // One fence per queue: a single fence signalled from several queues completes at the max of the
    // values, so the first queue to finish would open the gate for all of them.
    std::vector<ID3D12CommandQueue *> queues;
    {
        std::lock_guard<std::mutex> lock(g_queueMutex);
        for (const QueueEntry &e : g_queues)
            if (e.device == device || e.device == proxyDevice) { e.queue->AddRef(); queues.push_back(e.queue); }
    }
    for (ID3D12CommandQueue *queue : queues) {
        ID3D12Fence *fence = nullptr;
        if (SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))) && fence != nullptr) {
            set.Add(fence, 1);
            if (FAILED(queue->Signal(fence, 1)))
                Log(true, "Optimizer FPS core: retirement signal device error; resources remain pending");
            fence->Release();
        }
        else {
            // A failed fence allocation must not turn a partial gate into permission to release.
            std::lock_guard<std::mutex> lock(g_queueMutex);
            for (const QueueEntry &e : g_queues) if (e.queue == queue) set.Add(e.fence, UINT64_MAX);
            Log(true, "Optimizer FPS core: retirement fence device error; resources remain pending");
        }
        queue->Release();
    }
    return set;
}

GateSet SignalGateAll()
{
    std::vector<ID3D12Device *> devices;
    {
        std::lock_guard<std::mutex> lock(g_queueMutex);
        for (const QueueEntry &e : g_queues) {
            bool found = false;
            for (auto *device : devices) found |= device == e.device;
            if (!found) { e.device->AddRef(); devices.push_back(e.device); }
        }
    }
    GateSet all;
    for (auto *device : devices) { all.Append(SignalGate(device, nullptr)); device->Release(); }
    return all;
}

bool WaitRegisteredQueues(ID3D12Device *device, ID3D12Device *proxyDevice, ID3D12Fence *fence, UINT64 value)
{
    std::lock_guard<std::mutex> lock(g_queueMutex);
    bool any = false;
    for (const QueueEntry &e : g_queues)
        if (e.graphics && (e.device == device || e.device == proxyDevice)) { any |= SUCCEEDED(e.queue->Wait(fence, value)); }
    return any;
}

std::size_t CountQueues(ID3D12Device *device, ID3D12Device *proxyDevice, std::size_t *total)
{
    std::lock_guard<std::mutex> lock(g_queueMutex);
    std::size_t matching = 0;
    for (const QueueEntry &e : g_queues) matching += e.graphics && (e.device == device || e.device == proxyDevice) ? 1 : 0;
    if (total) *total = g_queues.size();
    return matching;
}

bool TimestampFrequency(ID3D12Device *device, ID3D12Device *proxyDevice, UINT64 *frequency)
{
    std::lock_guard<std::mutex> lock(g_queueMutex);
    const QueueEntry *pick = nullptr;
    for (const QueueEntry &e : g_queues) {
        if (!e.graphics) continue;
        if (e.device == device || e.device == proxyDevice) { pick = &e; break; }
        if (pick == nullptr) pick = &e;
    }
    if (pick == nullptr) return false;
    return SUCCEEDED(pick->queue->GetTimestampFrequency(frequency));
}

} // namespace ofps::core::gpu
