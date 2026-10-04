#pragma once
// Menu mode against a host whose NR runs on a queue other than the present queue (DLSS5-Reshade-AIO's async compute,
// bench12 --nr-list compute, the D3D11 bridge's D3D12 queue). The menu pass and the host's NR work share the NR feature
// and the guide snapshots; the present queue's order does not cover that queue, so GPU fences do:
//   entry: before a run's first pass the host queue signals `fence_` and the private queue waits for that value;
//   exit:  the hooked host evaluate that ends a run, BEFORE its model call, enqueues on the host queue a Wait for the
//          run's last pass (D3D12 queues are free-threaded; the list being recorded is submitted later, behind it).
//
// Correction C2 and fix round 1 (plan 2026-09-29-menu-mode): no pass without ordering.
// - Every host recording gets a fresh tag (private data; also when the list object is reused) and is "unobserved" until
//   a queue event reports THAT tag on a queue: ReShade's execute_command_list, or for a device ReShade does not wrap the
//   submit hook (queue_submit_hook.h, installed once UnobservedFor holds). OrderEntry answers WaitingForHostQueue (the
//   retryable blocker kMenuHostQueueBlocker) while the latest recording is unobserved or could not be tagged.
// - The observation must be at least one present old (NotePresent), see OrderEntry in the .cpp.
// - Exit: OrderExitNow waits on the CPU (at most kMenuExitCpuWaitMs) when no GPU Wait is possible (host queue unknown,
//   the Wait failed, the list cannot be tagged, a recording since the previous exit unobserved or on another queue) and
//   answers Quarantine on timeout: the caller withholds that one model call. ListExecuting is the safety net for an exit
//   list that runs on another queue than the one waited on: a GPU Wait on that queue first (fix round 2).
// These bounded waits at menu exit are menu mode's only CPU waits; none runs in the present event.
//
// Pointer contract: every queue passed here is the NATIVE ID3D12CommandQueue (ReShade's get_native(); the submit hook
// patches the native function, so its `this` is native too), so presentQueue,
// QueueDestroyed and the observed queue compare equal. Locking: mutex_ is a leaf lock. The D3D12 calls made under it
// (Signal, Wait, CreateFence, private data) are native calls that take no add-on or ReShade lock; lock order with the
// pipeline is g_mutex -> mutex_, never the reverse. CPU waits run outside mutex_.
#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>
#include <mutex>

namespace ofps::reshade {

inline constexpr const char *kMenuHostQueueBlocker = "waiting for the host's NR queue";
inline constexpr DWORD kMenuExitCpuWaitMs = 100;
// Presents of menu mode with the host's recordings unreported before the submit hook goes in (UnobservedFor).
inline constexpr std::uint64_t kMenuSubmitHookPresents = 8;

enum class MenuEntryOrder {
    Ordered,             // the private queue waits for a Signal on the host queue
    NotNeeded,           // the host's NR list runs on the present queue, or no host D3D12 list was recorded
    WaitingForHostQueue, // blocker (retryable): the latest recording is unobserved, observed this present, or untagged
    Failed,              // the Signal or the Wait failed (nothing waits for a failed Signal): stop menu mode
};

enum class MenuExitOrder {
    NotNeeded,     // nothing to order: same queue as the present queue, the last pass already completed, or no fence
    GpuWaitQueued, // the host queue waits for the last pass on the GPU
    CpuWaited,     // no GPU Wait was possible; the last pass completed within the bound
    // It did not (or the device was removed: UINT64_MAX never counts). OrderExitNow: the caller withholds the model call;
    // menu_pipeline_host.cpp quarantines menu mode only for a removed device (a slow GPU keeps it, final review I2).
    // ListExecuting: the list runs unordered; the pipeline quarantines. An armed event of a timed-out wait stays open.
    Quarantine,
};

class MenuHostQueue {
public:
    void NotePresent(); // present thread, every present before OrderEntry: the clock of the "one present old" rule
    // Host thread, after a successful watched evaluate. False: the list could not be tagged (entry blocked until a later
    // recording is tagged and seen executing). A null list (no D3D12 list: a D3D11 host) records nothing.
    bool ListRecorded(ID3D12CommandList *list);
    // The hooked host evaluate that ends a run, BEFORE its model call: order `list` (the list it records into) after
    // `fence` reaches `value` (a value whose Signal succeeded). A GPU Wait on the learned queue only when every recording
    // since the previous exit was tagged and seen executing on that one queue; otherwise a bounded CPU wait (outside the
    // lock). Quarantine: withhold this model call.
    MenuExitOrder OrderExitNow(ID3D12CommandList *list, ID3D12Fence *fence, UINT64 value, ID3D12CommandQueue *presentQueue,
                               DWORD timeoutMs = kMenuExitCpuWaitMs);
    // ReShade's execute_command_list (or the submit hook), before the native call: observes the latest recording's queue, and is the safety
    // net for an exit list that runs on another queue than the one OrderExitNow ordered: a GPU Wait on that queue
    // (GpuWaitQueued), or if it fails a bounded CPU wait (CpuWaited / Quarantine: log, menu mode off; not withheld).
    MenuExitOrder ListExecuting(ID3D12CommandQueue *queue, ID3D12CommandList *list);
    // Present thread, before a run's first pass. The entry fence lives on `device` (re-created when it changes).
    MenuEntryOrder OrderEntry(ID3D12Device *device, ID3D12CommandQueue *presentQueue, ID3D12CommandQueue *privateQueue);
    // True while OrderEntry would answer WaitingForHostQueue; enqueues nothing (the pipeline's blocker check each present).
    bool EntryWaiting() const;
    // True when host recordings were made and none was seen executing for at least `presents` presents (counted from the
    // first recording after the last observation of ANY tagged recording): no queue event reports the host's NR queue
    // (a device ReShade does not wrap). The trigger of the submit hook (queue_submit_hook.h).
    bool UnobservedFor(std::uint64_t presents) const;
    bool Separate(ID3D12CommandQueue *presentQueue) const; // a host queue is known and is not the present queue
    ID3D12CommandQueue *Queue() const;                     // for logging only: no lifetime guarantee
    void QueueDestroyed(ID3D12CommandQueue *queue);
    void Forget(); // the watched feature was released: drops everything, the entry fence too

private:
    bool WaitingLocked() const; // under mutex_: the WaitingForHostQueue rule
    MenuExitOrder WaitOutsideLock(std::unique_lock<std::mutex> &lock, ID3D12Fence *fence, UINT64 value, DWORD timeoutMs);

    mutable std::mutex mutex_;
    ID3D12CommandQueue *queue_ = nullptr;     // latest observed host queue; not owned: ReShade reports its destruction
    ID3D12CommandQueue *exitQueue_ = nullptr; // the queue the exit list is ordered on (safety net); not owned
    bool listSeen_ = false, tagFailed_ = false, observed_ = false; // observed_: the latest recording was seen executing
    bool historyBroken_ = false; // since the previous exit: a recording untagged, never seen executing, or on another queue
    bool unobserved_ = false;    // recordings since the last observation of any tagged one, the first at unobservedSince_
    std::uint64_t presents_ = 0, observedAt_ = 0, recordedTag_ = 0, exitTag_ = 0, nextTag_ = 0, unobservedSince_ = 0;
    Microsoft::WRL::ComPtr<ID3D12Fence> exitFence_, fence_;
    ID3D12Device *fenceDevice_ = nullptr; // the device fence_ was made on (kept alive by fence_ itself)
    UINT64 exitValue_ = 0, value_ = 0;
};

} // namespace ofps::reshade
