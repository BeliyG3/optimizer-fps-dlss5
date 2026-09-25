#pragma once
#include "core/api/ofps_core.h"
#include "core/gpu/graveyard.h"
#include "core/gpu/submission_tags.h"
#include <cstdint>
#include <mutex>
#include <vector>
namespace ofps::core::gpu {
struct SubmissionEntry {
    ID3D12CommandQueue *queue = nullptr;
    ID3D12CommandList *list = nullptr;
    std::uint64_t serial = 0;
    std::uint64_t asyncTag = 0;
    std::uint64_t flowTag = 0;
};
constexpr std::uint32_t kSubmissionRing = 256;
class Submission {
public:
    enum class Signal { All, Settled };
    Submission() = default;
    ~Submission();
    Submission(const Submission &) = delete;
    Submission &operator=(const Submission &) = delete;
    bool Track(ID3D12Device *device, ID3D12CommandQueue *queue);
    void Untrack(ID3D12CommandQueue *queue);
    void Push(ID3D12CommandQueue *queue, ID3D12CommandList *list);
    std::uint32_t Drain(SubmissionEntry *out, std::uint32_t capacity, Signal policy);
    // Borrowed until the next Drain (under Ctx().mutex); pool/gate retains it.
    OfpsFencePoint UsePoint(ID3D12CommandList *list);
    bool NextSubmission(ID3D12CommandQueue *queue, OfpsFencePoint *point) const;
    void PendingGate(ID3D12Device *device, ID3D12Device *proxyDevice, GateSet *gate) const;
    void PendingGateAll(GateSet *gate) const;
    std::uint64_t Dropped() const;
private:
    struct Tracked {
        ID3D12Device *device = nullptr;
        ID3D12CommandQueue *queue = nullptr;
        ID3D12Fence *fence = nullptr;
        std::uint64_t pushed = 0, settled = 0, signalled = 0;
    };
    struct Recording {
        ID3D12Device *device = nullptr;
        ID3D12Fence *fence = nullptr;
        std::uint64_t tag = 0;
        ID3D12CommandQueue *queue = nullptr;
        std::uint64_t serial = 0;
        std::uint64_t createdEval = 0;
        bool signalled = false;
    };
    void SignalRecordings(const Tracked &queue, std::uint64_t target);
    Tracked *Find(ID3D12CommandQueue *queue);
    const Tracked *Find(ID3D12CommandQueue *queue) const;
    mutable std::mutex mutex_;
    std::vector<Tracked> queues_;
    std::vector<Recording> recordings_;
    std::uint64_t nextTag_ = 0;
    SubmissionEntry ring_[kSubmissionRing];
    std::uint32_t head_ = 0, count_ = 0;
    std::uint64_t dropped_ = 0;
};
}
