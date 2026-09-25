#pragma once
#include "core/flow/Backend.h"
#include <string>

namespace ofps::core::flow {
// Per-temporal-machine frame state; the NVOFA session itself lives for the process.
class FrameState final {
public:
    explicit FrameState(Backend* backend = nullptr);
    ~FrameState() { Reset(); }
    bool Acquire(ID3D12Device* device, std::uint32_t width, std::uint32_t height);
    bool HasSession() const { return session_ && !failed_; }
    std::uint32_t Width() const { return backend_->Width(session_); }
    std::uint32_t Height() const { return backend_->Height(session_); }
    std::uint32_t Grid() const { return backend_->Grid(session_); }
    ID3D12Resource* Now() const { return backend_->Now(session_); }
    ID3D12Resource* Then() const { return backend_->Then(session_); }
    ID3D12Resource* Field() const { return backend_->Field(session_); }
    const std::string& Problem() const { return problem_; }
    void Reset();
    void NowWritten(bool written) { nowWritten_ = written; }
    void ThenWritten(bool written) { thenWritten_ = written; }
    bool Arm(ID3D12GraphicsCommandList* list);
    std::uint64_t ArmTag(); // also used by the fake backend test without a D3D list
    void Submitted(std::uint64_t tag, ID3D12CommandQueue* queue);
    bool Ready() const { return HasSession() && nowWritten_ && thenWritten_ && submittedQueue_; }
    ID3D12CommandQueue* Queue() const { return submittedQueue_; }
    bool Execute() { return Ready() && backend_->Execute(session_, submittedQueue_); }
    void Consumed() { DropQueue(); nowWritten_ = false; }
    void Failed(const char* why) { problem_ = why; }
    void Disable(const char* why) { problem_ = why; failed_ = true; Reset(); }
    void Succeeded() { problem_.clear(); }

private:
    Backend* backend_ = nullptr;
    void* session_ = nullptr;
    std::string problem_;
    bool failed_ = false, nowWritten_ = false, thenWritten_ = false;
    std::uint64_t pendingTag_ = 0;
    ID3D12CommandQueue* submittedQueue_ = nullptr;
    void DropQueue();
};
} // namespace ofps::core::flow
