#include "core/flow/FrameState.h"
#include "core/gpu/submission_tags.h"
#include <atomic>

namespace ofps::core::flow {
namespace { std::atomic<std::uint64_t> nextTag{0}; }
FrameState::FrameState(Backend* backend) : backend_(backend ? backend : &DriverBackend()) {}

bool FrameState::Acquire(ID3D12Device* device, std::uint32_t width, std::uint32_t height) {
    if (failed_) return false;
    if (!session_ && !failed_) {
        session_ = backend_->Acquire(device, width, height, problem_);
        failed_ = session_ == nullptr;
    }
    return session_ != nullptr && !failed_;
}
void FrameState::Reset() {
    nowWritten_ = thenWritten_ = false;
    pendingTag_ = 0;
    DropQueue();
}
void FrameState::DropQueue() {
    if (submittedQueue_) backend_->ReleaseQueue(submittedQueue_);
    submittedQueue_ = nullptr;
}
bool FrameState::Arm(ID3D12GraphicsCommandList* list) {
    if (!list) return false;
    const auto tag = ArmTag();
    if (!tag) return false;
    if (FAILED(list->SetPrivateData(gpu::kTemporalFlowTag, sizeof(tag), &tag))) {
        problem_ = "optical flow command list cannot be tagged";
        pendingTag_ = 0;
        return false;
    }
    return true;
}
std::uint64_t FrameState::ArmTag() {
    if (!HasSession() || !nowWritten_ || !thenWritten_) return 0;
    pendingTag_ = ++nextTag;
    DropQueue();
    return pendingTag_;
}
void FrameState::Submitted(std::uint64_t tag, ID3D12CommandQueue* queue) {
    if (tag && tag == pendingTag_ && queue) {
        DropQueue();
        backend_->RetainQueue(queue);
        submittedQueue_ = queue;
    }
}
} // namespace ofps::core::flow
