#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
#include <cstddef>
#include <cstdint>
#include <vector>
namespace ofps::core::gpu {
bool RegisterQueue(ID3D12Device *device, ID3D12CommandQueue *queue);
void SetQueueRegistrationSuppressed(bool suppressed);
void UnregisterQueue(ID3D12CommandQueue *queue);
ID3D12CommandQueue *RetainRegisteredQueue(ID3D12CommandQueue *queue);
bool WaitForGpu(ID3D12Device *device, ID3D12Device *proxyDevice);
// Graphics (DIRECT) queues only: the background mode signals and waits where the host submits its
// frames; an idle compute queue would reach a shared fence value early. CountQueues counts them too.
bool SignalRegisteredQueues(ID3D12Device *device, ID3D12Device *proxyDevice, ID3D12Fence *fence,
                            UINT64 value);
bool WaitRegisteredQueues(ID3D12Device *device, ID3D12Device *proxyDevice, ID3D12Fence *fence, UINT64 value);
std::size_t CountQueues(ID3D12Device *device, ID3D12Device *proxyDevice, std::size_t *total);
bool TimestampFrequency(ID3D12Device *device, ID3D12Device *proxyDevice, UINT64 *frequency);
bool WaitFenceValue(ID3D12Fence *fence, UINT64 value, HANDLE event, DWORD milliseconds);
constexpr DWORD kGpuWaitMilliseconds = 3000;
constexpr std::uint64_t kDeferredReleaseEvaluates = 16;
struct FenceGate {
    ID3D12Fence *fence = nullptr;
    UINT64 value = 0;
};
class GateSet {
public:
    GateSet() = default;
    GateSet(const GateSet &other) { Assign(other); }
    GateSet(GateSet &&other) noexcept : gates_(std::move(other.gates_)) { other.gates_.clear(); }
    GateSet &operator=(const GateSet &other) {
        if (this != &other) {
            Clear();
            Assign(other);
        }
        return *this;
    }
    GateSet &operator=(GateSet &&other) noexcept {
        if (this != &other) {
            Clear();
            gates_ = std::move(other.gates_);
            other.gates_.clear();
        }
        return *this;
    }
    ~GateSet() { Clear(); }
    void Add(ID3D12Fence *fence, UINT64 value);
    void Clear();
    void Append(const GateSet &other);
    bool Empty() const { return gates_.empty(); }
    std::size_t Size() const { return gates_.size(); }
    bool Completed() const;
    void Wait(DWORD milliseconds) const;

private:
    void Assign(const GateSet &other);
    std::vector<FenceGate> gates_;
};
GateSet SignalGate(ID3D12Device *device, ID3D12Device *proxyDevice);
GateSet SignalGateAll();
} // namespace ofps::core::gpu
