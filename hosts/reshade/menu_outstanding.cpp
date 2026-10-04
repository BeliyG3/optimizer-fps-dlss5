#include "hosts/reshade/menu_outstanding.h"
#include <cstdint>

namespace ofps::reshade {
namespace {
// UINT64_MAX or a removed device proves nothing (menu_fence.h).
bool Complete(ID3D12Fence *fence, UINT64 value) { return MenuFencePassed(fence, value); }
} // namespace

MenuPassWait MenuProvePass(const MenuPassProof &proof, DWORD ms) {
    if (proof.unfenced) return MenuPassWait::Unfenced;
    if (!proof.fence) return MenuPassWait::Done;
    if (MenuFenceRemoved(proof.fence.Get())) return MenuPassWait::Removed;
    if (Complete(proof.fence.Get(), proof.value)) return MenuPassWait::Done;
    if (!proof.wait) return MenuPassWait::NotDone;
    const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event) return MenuPassWait::TimedOut;
    if (FAILED(proof.fence->SetEventOnCompletion(proof.value, event))) { // never armed: nothing can set it
        CloseHandle(event);
        if (Complete(proof.fence.Get(), proof.value)) return MenuPassWait::Done;
        return MenuFenceRemoved(proof.fence.Get()) ? MenuPassWait::Removed : MenuPassWait::TimedOut;
    }
    if (WaitForSingleObject(event, ms) == WAIT_OBJECT_0 && Complete(proof.fence.Get(), proof.value)) {
        CloseHandle(event);
        return MenuPassWait::Waited;
    }
    // Armed: the fence may still set the event later, so it is kept (leaked).
    return MenuFenceRemoved(proof.fence.Get()) ? MenuPassWait::Removed : MenuPassWait::TimedOut;
}

bool MenuOutstandingPass::Recover(ID3D12Device *device, ID3D12CommandQueue *privateQueue) {
    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    // Queues run in order: a Signal enqueued now on the private queue completes only after the pass before it.
    if (device && privateQueue && SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))) &&
        SUCCEEDED(privateQueue->Signal(fence.Get(), 1))) {
        Hold(fence.Get(), 1);
        return true;
    }
    unfenced_ = true;
    fence_.Reset();
    value_ = 0;
    return false;
}

void MenuOutstandingPass::Hold(ID3D12Fence *fence, UINT64 value) {
    if (unfenced_ || !fence) return;
    if (fence_.Get() == fence && value_ == value) return; // the same pass: an expired wait stays expired
    fence_ = fence;
    value_ = value;
    expired_ = false;
}

void MenuOutstandingPass::NotePass(ID3D12Fence *f2, UINT64 value) {
    last_ = f2;
    lastValue_ = value;
}

MenuPassProof MenuOutstandingPass::ProofFor(bool forceWait) const {
    MenuPassProof proof;
    if (unfenced_) {
        proof.unfenced = true;
    } else if (fence_) {
        proof.fence = fence_;
        proof.value = value_;
        proof.wait = forceWait || !expired_;
        proof.outstanding = true;
    } else if (last_ && !Complete(last_.Get(), lastValue_)) {
        proof.fence = last_;
        proof.value = lastValue_;
        proof.wait = true;
    }
    return proof;
}

MenuPassGate MenuOutstandingPass::Settle(const MenuPassProof &proof, MenuPassWait result) {
    switch (result) {
    case MenuPassWait::Done:
    case MenuPassWait::Waited:
        if (!proof.outstanding || fence_ != proof.fence || value_ != proof.value) return MenuPassGate::Call;
        fence_.Reset();
        value_ = 0;
        expired_ = false;
        return MenuPassGate::Resumed;
    case MenuPassWait::TimedOut: // a live device: slow, not broken (final review, Claude I2)
    case MenuPassWait::Removed:
        Hold(proof.fence.Get(), proof.value);
        expired_ = fence_ != nullptr;
        return result == MenuPassWait::Removed ? MenuPassGate::Quarantine : MenuPassGate::Slow;
    case MenuPassWait::NotDone:
    case MenuPassWait::Unfenced: break;
    }
    return MenuPassGate::Withhold;
}

void MenuOutstandingPass::ForgetCompletedLast() {
    if (!last_ || !Complete(last_.Get(), lastValue_)) return;
    last_.Reset();
    lastValue_ = 0;
}

void MenuOutstandingPass::Clear() {
    fence_.Reset();
    last_.Reset();
    value_ = lastValue_ = 0;
    unfenced_ = expired_ = false;
}

} // namespace ofps::reshade
