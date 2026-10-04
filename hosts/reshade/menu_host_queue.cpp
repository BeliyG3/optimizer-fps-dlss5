#include "hosts/reshade/menu_host_queue.h"
#include "hosts/reshade/menu_fence.h"

namespace ofps::reshade {
namespace {
// Two tags, because the exit evaluate's list is also a recording: ListRecorded (after the model call) must not overwrite
// the exit tag that OrderExitNow (before it) set on the same list.
// {7C3E91A4-2B6D-4F05-9A8E-3D1C5B7F2E60}
constexpr GUID kMenuListTag = {0x7c3e91a4, 0x2b6d, 0x4f05, {0x9a, 0x8e, 0x3d, 0x1c, 0x5b, 0x7f, 0x2e, 0x60}};
// {7C3E91A5-2B6D-4F05-9A8E-3D1C5B7F2E60}
constexpr GUID kMenuExitTag = {0x7c3e91a5, 0x2b6d, 0x4f05, {0x9a, 0x8e, 0x3d, 0x1c, 0x5b, 0x7f, 0x2e, 0x60}};

std::uint64_t TagOf(ID3D12CommandList *list, REFGUID guid) {
    std::uint64_t tag = 0;
    UINT size = sizeof(tag);
    return list && SUCCEEDED(list->GetPrivateData(guid, &size, &tag)) && size == sizeof(tag) ? tag : 0;
}
// A fresh tag every call, also for a list object that carried one before (a reused list is a new recording).
std::uint64_t NewTag(ID3D12CommandList *list, REFGUID guid, std::uint64_t &next) {
    const std::uint64_t tag = ++next;
    return list && SUCCEEDED(list->SetPrivateData(guid, sizeof(tag), &tag)) ? tag : 0;
}
// True when `fence` reaches `value` within `ms` on a live device (a removed device's UINT64_MAX proves nothing).
// Final review (Codex I2): an event armed by SetEventOnCompletion is closed only once the wait saw it set; after a
// timeout the fence may still set it later, so it is kept (leaked), as MenuProvePass does.
bool Reaches(ID3D12Fence *fence, UINT64 value, DWORD ms) {
    if (MenuFencePassed(fence, value)) return true;
    if (MenuFenceRemoved(fence)) return false;
    const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event) return false;
    if (FAILED(fence->SetEventOnCompletion(value, event))) { // never armed: nothing can set it
        CloseHandle(event);
        return MenuFencePassed(fence, value);
    }
    if (WaitForSingleObject(event, ms) == WAIT_OBJECT_0) {
        CloseHandle(event);
        return MenuFencePassed(fence, value);
    }
    return MenuFencePassed(fence, value); // armed and timed out: the event stays open
}
} // namespace

void MenuHostQueue::NotePresent() {
    std::lock_guard lock(mutex_);
    ++presents_;
}

bool MenuHostQueue::ListRecorded(ID3D12CommandList *list) {
    if (!list) return true;
    std::lock_guard lock(mutex_);
    if (listSeen_ && !observed_) historyBroken_ = true; // the previous recording was never seen executing
    listSeen_ = true;
    observed_ = false; // a newer recording invalidates the older observation
    if (!unobserved_) {
        unobserved_ = true;
        unobservedSince_ = presents_;
    }
    recordedTag_ = NewTag(list, kMenuListTag, nextTag_);
    tagFailed_ = recordedTag_ == 0;
    if (tagFailed_) historyBroken_ = true;
    return !tagFailed_;
}

MenuExitOrder MenuHostQueue::WaitOutsideLock(std::unique_lock<std::mutex> &lock, ID3D12Fence *fence, UINT64 value, DWORD timeoutMs) {
    Microsoft::WRL::ComPtr<ID3D12Fence> hold = fence; // the members may change while the lock is released
    lock.unlock();
    const DWORD bound = timeoutMs < kMenuExitCpuWaitMs ? timeoutMs : kMenuExitCpuWaitMs;
    return Reaches(hold.Get(), value, bound) ? MenuExitOrder::CpuWaited : MenuExitOrder::Quarantine;
}

MenuExitOrder MenuHostQueue::OrderExitNow(ID3D12CommandList *list, ID3D12Fence *fence, UINT64 value,
                                          ID3D12CommandQueue *presentQueue, DWORD timeoutMs) {
    std::unique_lock lock(mutex_);
    exitTag_ = 0;
    exitFence_.Reset();
    exitValue_ = 0;
    exitQueue_ = nullptr;
    // The learned queue predicts where the exit list will run only when every recording before it (since the previous
    // exit) was tagged and seen executing on that one queue; the exit list itself is necessarily still unobserved.
    const bool trusted = queue_ && observed_ && !tagFailed_ && !historyBroken_;
    historyBroken_ = false; // the next exit judges the recordings after this one
    if (!fence || MenuFencePassed(fence, value)) return MenuExitOrder::NotNeeded;
    // The tag only arms the safety net in ListExecuting; without it the queue the list runs on cannot be checked.
    const std::uint64_t tag = NewTag(list, kMenuExitTag, nextTag_);
    MenuExitOrder order = MenuExitOrder::NotNeeded;
    if (!tag || !trusted) order = MenuExitOrder::CpuWaited;
    else if (queue_ == presentQueue) exitQueue_ = presentQueue; // the present queue waited for the pass before presenting
    else if (SUCCEEDED(queue_->Wait(fence, value))) {
        // Enqueued now, from the evaluate thread: the list being recorded is submitted later, so it runs behind this Wait.
        exitQueue_ = queue_;
        order = MenuExitOrder::GpuWaitQueued;
    } else order = MenuExitOrder::CpuWaited;
    if (order == MenuExitOrder::CpuWaited) return WaitOutsideLock(lock, fence, value, timeoutMs); // no net needed after
    exitTag_ = tag;
    exitFence_ = fence;
    exitValue_ = value;
    return order;
}

MenuExitOrder MenuHostQueue::ListExecuting(ID3D12CommandQueue *queue, ID3D12CommandList *list) {
    std::unique_lock lock(mutex_);
    if (!queue || !list) return MenuExitOrder::NotNeeded;
    const std::uint64_t tag = TagOf(list, kMenuListTag);
    if (tag) unobserved_ = false; // the host's recordings are reported: the submit hook's trigger is off
    if (tag && tag == recordedTag_) {
        if (queue_ && queue_ != queue) historyBroken_ = true; // recordings on two queues since the previous exit
        queue_ = queue;
        observed_ = true;
        observedAt_ = presents_;
    }
    const std::uint64_t exitTag = TagOf(list, kMenuExitTag);
    if (!exitTag || exitTag != exitTag_ || !exitFence_) return MenuExitOrder::NotNeeded;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence = exitFence_;
    const UINT64 value = exitValue_;
    const bool ordered = queue == exitQueue_;
    exitTag_ = 0;
    exitFence_.Reset();
    exitValue_ = 0;
    exitQueue_ = nullptr;
    if (ordered) return MenuExitOrder::NotNeeded;
    // The exit list is about to run on a queue OrderExitNow did not order. A GPU Wait on THIS queue, enqueued now, lands
    // before the native ExecuteCommandLists that follows the event, so it orders the list without a CPU wait.
    if (SUCCEEDED(queue->Wait(fence.Get(), value))) return MenuExitOrder::GpuWaitQueued;
    // The Wait failed: waiting here, before the native call, still orders it. The submission can no longer be withheld
    // (the event cannot cancel the native call); a timeout means the device is effectively broken, and the caller only
    // logs Quarantine and turns menu mode off for the session.
    return WaitOutsideLock(lock, fence.Get(), value, kMenuExitCpuWaitMs);
}

bool MenuHostQueue::WaitingLocked() const {
    if (tagFailed_) return true;
    if (!listSeen_) return false; // no host D3D12 list: nothing to order
    // Race with the native submit: ReShade's execute_command_list fires just BEFORE the native ExecuteCommandLists, on
    // the host's submit thread. A Signal enqueued on that queue right after the event could land ahead of the host's NR
    // list and let the menu pass run first. So the observation must be at least one present old: the native call
    // follows the event directly and waits on nothing of ours, so by the next present it has been made (entry itself
    // also needs >= 150 ms without a host evaluate).
    return !observed_ || !queue_ || observedAt_ >= presents_;
}

bool MenuHostQueue::EntryWaiting() const {
    std::lock_guard lock(mutex_);
    return WaitingLocked();
}

bool MenuHostQueue::UnobservedFor(std::uint64_t presents) const {
    std::lock_guard lock(mutex_);
    return unobserved_ && presents_ - unobservedSince_ >= presents;
}

MenuEntryOrder MenuHostQueue::OrderEntry(ID3D12Device *device, ID3D12CommandQueue *presentQueue, ID3D12CommandQueue *privateQueue) {
    std::lock_guard lock(mutex_);
    if (WaitingLocked()) return MenuEntryOrder::WaitingForHostQueue;
    if (!listSeen_) return MenuEntryOrder::NotNeeded; // no host D3D12 list: nothing to order
    if (queue_ == presentQueue) return MenuEntryOrder::NotNeeded; // one queue orders it
    if (!device || !privateQueue) return MenuEntryOrder::Failed;
    if (!fence_ || fenceDevice_ != device) { // a fence belongs to one device: never Signal/Wait it across devices
        fence_.Reset();
        fenceDevice_ = nullptr;
        value_ = 0;
        if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)))) return MenuEntryOrder::Failed;
        fenceDevice_ = device;
    }
    if (FAILED(queue_->Signal(fence_.Get(), value_ + 1))) return MenuEntryOrder::Failed; // no Wait for a failed Signal
    ++value_;
    return SUCCEEDED(privateQueue->Wait(fence_.Get(), value_)) ? MenuEntryOrder::Ordered : MenuEntryOrder::Failed;
}

bool MenuHostQueue::Separate(ID3D12CommandQueue *presentQueue) const {
    std::lock_guard lock(mutex_);
    return queue_ && queue_ != presentQueue;
}

ID3D12CommandQueue *MenuHostQueue::Queue() const {
    std::lock_guard lock(mutex_);
    return queue_;
}

void MenuHostQueue::QueueDestroyed(ID3D12CommandQueue *queue) {
    std::lock_guard lock(mutex_);
    if (queue == queue_) {
        queue_ = nullptr;
        observed_ = false;
    }
    if (queue == exitQueue_) exitQueue_ = nullptr; // the exit list then gets the CPU safety net on any queue
}

void MenuHostQueue::Forget() {
    std::lock_guard lock(mutex_);
    queue_ = exitQueue_ = nullptr;
    listSeen_ = tagFailed_ = observed_ = historyBroken_ = unobserved_ = false;
    observedAt_ = recordedTag_ = exitTag_ = unobservedSince_ = 0;
    exitFence_.Reset();
    exitValue_ = 0;
    fence_.Reset(); // releases the device reference: a later device (or a recreated one) gets a fresh fence
    fenceDevice_ = nullptr;
    value_ = 0;
}

} // namespace ofps::reshade
