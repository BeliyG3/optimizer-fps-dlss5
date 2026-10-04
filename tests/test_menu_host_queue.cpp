// Menu mode's fences against a host NR queue other than the present queue (menu_host_queue.h), on WARP: the entry side
// (per-recording observation, the one-present rule, blockers, failures). Exit: test_menu_host_queue_exit.cpp; real GPU
// data order and the device-bound fence: test_menu_host_queue_order.cpp. The proxy-list (hook) versus native-list
// (queue event) tag match and the event-before-native-call order are the Task 9 bench case mp_model_cl.
#include "test_menu_host_queue_support.h"
#include "test_menu_host_queue_fakes.h"
#include "hosts/reshade/menu_host_queue.h"
#include <string>

using namespace ofps::reshade;
using namespace menutest;
namespace {
constexpr auto kDirect = D3D12_COMMAND_LIST_TYPE_DIRECT;
constexpr auto kCompute = D3D12_COMMAND_LIST_TYPE_COMPUTE;

// A host that runs NR on the present queue needs no fence: that queue's order already covers it.
void TestSameQueue(ID3D12Device *device) {
    auto present = Queue(device, kDirect), privateQueue = Queue(device, kDirect);
    auto gate = Fence(device), f2 = Fence(device);
    ComPtr<ID3D12CommandAllocator> allocator;
    auto list = List(device, kDirect, allocator);
    MenuHostQueue q;
    q.ListRecorded(list.Get());
    q.ListExecuting(present.Get(), list.Get());
    q.NotePresent();
    present->Wait(gate.Get(), 1);
    Check(!q.Separate(present.Get()) && q.OrderEntry(device, present.Get(), privateQueue.Get()) == MenuEntryOrder::NotNeeded,
          "nothing to order");
    Check(Free(device, privateQueue.Get(), 2000), "the private queue is not held by the present queue");
    gate->Signal(1);
    Check(Free(device, present.Get(), 2000), "the present queue drains before it is released");
    Check(q.OrderExitNow(list.Get(), f2.Get(), 1, present.Get()) == MenuExitOrder::NotNeeded, "no exit Wait on the present queue");
    Check(q.ListExecuting(present.Get(), list.Get()) == MenuExitOrder::NotNeeded, "and no CPU wait when it runs there");
    MenuHostQueue unknown;
    Check(unknown.OrderEntry(device, present.Get(), privateQueue.Get()) == MenuEntryOrder::NotNeeded,
          "no host D3D12 list recorded: nothing to order, nothing enqueued");
    MenuHostQueue d3d11;
    Check(d3d11.ListRecorded(nullptr) && d3d11.OrderEntry(device, present.Get(), privateQueue.Get()) == MenuEntryOrder::NotNeeded,
          "a host evaluate without a D3D12 list leaves nothing to order");
}

// C2 + fix round 1: a recording is a blocker until seen executing, and the observation must be one present old.
void TestUnobservedQueue(ID3D12Device *device) {
    auto present = Queue(device, kDirect), host = Queue(device, kCompute), privateQueue = Queue(device, kDirect);
    ComPtr<ID3D12CommandAllocator> allocator;
    auto list = List(device, kCompute, allocator);
    MenuHostQueue q;
    Check(!q.EntryWaiting(), "no host list recorded: entry does not wait");
    q.ListRecorded(list.Get());
    Check(q.EntryWaiting(), "EntryWaiting (the pipeline's blocker check, enqueues nothing) answers like OrderEntry");
    Check(q.OrderEntry(device, present.Get(), privateQueue.Get()) == MenuEntryOrder::WaitingForHostQueue,
          "an unobserved host queue is the blocker, not a silent pass");
    Check(Free(device, privateQueue.Get(), 2000), "the blocker enqueues nothing on the private queue");
    q.ListExecuting(host.Get(), list.Get());
    Check(q.EntryWaiting(), "EntryWaiting: observed in this very present still waits");
    Check(q.OrderEntry(device, present.Get(), privateQueue.Get()) == MenuEntryOrder::WaitingForHostQueue,
          "observed in this very present: the host's native submit may not have happened yet");
    q.NotePresent();
    Check(!q.EntryWaiting(), "EntryWaiting: one present later the blocker is gone");
    Check(Free(device, privateQueue.Get(), 2000), "EntryWaiting enqueued nothing");
    Check(q.OrderEntry(device, present.Get(), privateQueue.Get()) == MenuEntryOrder::Ordered, "one present later it is ordered");
    Check(Free(device, privateQueue.Get(), 2000), "the host queue is idle: the private queue runs on");
    q.QueueDestroyed(host.Get());
    Check(q.EntryWaiting(), "EntryWaiting: a destroyed host queue waits again");
    Check(q.OrderEntry(device, present.Get(), privateQueue.Get()) == MenuEntryOrder::WaitingForHostQueue,
          "a destroyed host queue must be observed again");
    q.Forget();
    Check(!q.EntryWaiting(), "EntryWaiting: nothing after Forget");
    Check(q.OrderEntry(device, present.Get(), privateQueue.Get()) == MenuEntryOrder::NotNeeded, "Forget drops the pending host list");
    Check(std::string(kMenuHostQueueBlocker) == "waiting for the host's NR queue", "the blocker text the tab shows");
}

// Fix round 1 (Codex C2 / review I1): each recording gets a fresh tag; only ITS execution counts. NR moving from the
// present queue to another queue is ordered against the new queue.
void TestNewRecording(ID3D12Device *device) {
    auto present = Queue(device, kDirect), realHost = Queue(device, kCompute), realPrivate = Queue(device, kDirect);
    FailingQueue host(realHost.Get(), false, false), privateSpy(realPrivate.Get(), false, false); // record only
    ID3D12CommandQueue *privateQueue = &privateSpy;
    ComPtr<ID3D12CommandAllocator> allocatorA, allocatorB;
    auto a = List(device, kCompute, allocatorA), b = List(device, kCompute, allocatorB);
    MenuHostQueue q;
    q.ListRecorded(a.Get());
    q.ListExecuting(present.Get(), a.Get());
    q.NotePresent();
    Check(q.OrderEntry(device, present.Get(), privateQueue) == MenuEntryOrder::NotNeeded, "NR on the present queue");
    q.ListRecorded(b.Get());
    Check(q.OrderEntry(device, present.Get(), privateQueue) == MenuEntryOrder::WaitingForHostQueue,
          "a newer recording invalidates the older observation");
    q.ListExecuting(present.Get(), a.Get());
    q.NotePresent();
    Check(q.OrderEntry(device, present.Get(), privateQueue) == MenuEntryOrder::WaitingForHostQueue,
          "an older recording executing again does not count");
    q.ListRecorded(a.Get()); // the list object is reused for a new recording
    q.ListExecuting(&host, b.Get());
    q.NotePresent();
    Check(q.OrderEntry(device, present.Get(), privateQueue) == MenuEntryOrder::WaitingForHostQueue,
          "a reused list gets a fresh tag: the other list's execution does not count");
    Check(host.signals == 0 && privateSpy.waits == 0, "no blocker enqueued anything");
    q.ListExecuting(&host, a.Get());
    q.NotePresent();
    Check(q.OrderEntry(device, present.Get(), privateQueue) == MenuEntryOrder::Ordered && q.Queue() == &host,
          "ordered against the queue the latest recording ran on");
    Check(host.signals == 1 && privateSpy.waits == 1 && host.signalFence && host.signalFence == privateSpy.waitFence &&
              host.signalValue == privateSpy.waitValue,
          "the private queue waits for exactly the value the new host queue signalled");
    Check(Free(device, realPrivate.Get(), 2000), "the host queue is idle: the private queue runs on");
}

// C2: a host list that cannot be tagged is the same blocker, even with a queue observed before; a later tagged
// recording lifts it only once seen executing.
void TestTagFailure(ID3D12Device *device) {
    auto present = Queue(device, kDirect), host = Queue(device, kCompute), privateQueue = Queue(device, kDirect);
    ComPtr<ID3D12CommandAllocator> allocator;
    auto list = List(device, kCompute, allocator);
    UntaggableList untaggable;
    MenuHostQueue q;
    q.ListRecorded(list.Get());
    q.ListExecuting(host.Get(), list.Get());
    q.NotePresent();
    Check(!q.ListRecorded(&untaggable), "a failed tag is reported");
    Check(q.EntryWaiting(), "EntryWaiting: a failed tag waits");
    Check(q.OrderEntry(device, present.Get(), privateQueue.Get()) == MenuEntryOrder::WaitingForHostQueue, "a failed tag blocks entry");
    Check(Free(device, privateQueue.Get(), 2000), "and enqueues nothing");
    q.ListRecorded(list.Get());
    q.NotePresent();
    Check(q.OrderEntry(device, present.Get(), privateQueue.Get()) == MenuEntryOrder::WaitingForHostQueue,
          "a tagged recording lifts it only once seen executing");
    q.ListExecuting(host.Get(), list.Get());
    q.NotePresent();
    Check(q.OrderEntry(device, present.Get(), privateQueue.Get()) == MenuEntryOrder::Ordered, "then it is ordered");
}

// Never a Wait for a value whose Signal failed; a failed private Wait fails too, and the next entry still waits for
// its own Signal, not the earlier one (value_ stays consistent).
void TestEntryFailures(ID3D12Device *device) {
    auto present = Queue(device, kDirect), real = Queue(device, kCompute), privateQueue = Queue(device, kDirect);
    ComPtr<ID3D12CommandAllocator> allocator;
    auto list = List(device, kCompute, allocator);
    FailingQueue badHost(real.Get(), true, false), badPrivate(privateQueue.Get(), false, true);
    FailingQueue host(real.Get(), false, false), privateSpy(privateQueue.Get(), false, false); // record only
    MenuHostQueue q;
    q.ListRecorded(list.Get());
    q.ListExecuting(&badHost, list.Get());
    q.NotePresent();
    Check(q.OrderEntry(device, present.Get(), privateQueue.Get()) == MenuEntryOrder::Failed && badHost.signals == 1, "a failed Signal fails");
    Check(Free(device, privateQueue.Get(), 2000), "and the private queue got no Wait");
    MenuHostQueue r;
    r.ListRecorded(list.Get());
    r.ListExecuting(&host, list.Get());
    r.NotePresent();
    Check(r.OrderEntry(device, present.Get(), &badPrivate) == MenuEntryOrder::Failed && badPrivate.waits == 1, "a failed private Wait fails");
    const UINT64 failedRun = host.signalValue;
    Check(r.OrderEntry(device, present.Get(), &privateSpy) == MenuEntryOrder::Ordered, "a first run is ordered");
    const UINT64 firstRun = host.signalValue;
    Check(firstRun == failedRun + 1 && privateSpy.waitValue == firstRun && privateSpy.waitFence == host.signalFence,
          "the first run waits for its own Signal");
    Check(r.OrderEntry(device, present.Get(), &privateSpy) == MenuEntryOrder::Ordered, "a second run is ordered");
    Check(host.signalValue == firstRun + 1 && privateSpy.waitValue == firstRun + 1,
          "the second run waits for its own Signal, not the first run's (already passed)");
    Check(Free(device, privateQueue.Get(), 2000), "the host queue is idle: nothing holds the private queue");
}

// The submit hook's trigger (menu_host_queue.h UnobservedFor): recordings that no queue event ever reports for N presents
// (a host whose D3D12 device ReShade does not wrap); any tagged recording seen executing ends it.
void TestUnobservedFor(ID3D12Device *device) {
    auto host = Queue(device, kCompute);
    ComPtr<ID3D12CommandAllocator> allocatorA, allocatorB;
    auto a = List(device, kCompute, allocatorA), b = List(device, kCompute, allocatorB);
    MenuHostQueue q;
    Check(!q.UnobservedFor(0), "no recording: nothing is unobserved");
    q.ListRecorded(a.Get());
    Check(q.UnobservedFor(0) && !q.UnobservedFor(1), "a recording starts the count at this present");
    for (int i = 0; i < 3; ++i) { q.NotePresent(); q.ListRecorded(a.Get()); }
    Check(q.UnobservedFor(3) && !q.UnobservedFor(4), "new recordings do not restart the count");
    q.ListRecorded(b.Get());
    q.ListExecuting(host.Get(), a.Get()); // an older recording: the queue event channel works
    Check(!q.UnobservedFor(0), "any tagged recording seen executing ends the count");
    q.NotePresent();
    q.ListRecorded(a.Get());
    q.NotePresent();
    Check(q.UnobservedFor(1) && !q.UnobservedFor(2), "a recording after that starts a new count");
    q.Forget();
    Check(!q.UnobservedFor(0), "Forget drops the count");
}
} // namespace

int main() {
    coretest::WarpDevice w;
    if (!coretest::CreateWarpDevice(w)) { std::fprintf(stderr, "FAIL: WARP device\n"); return 1; }
    TestSameQueue(w.device.Get());
    TestUnobservedQueue(w.device.Get());
    TestNewRecording(w.device.Get());
    TestTagFailure(w.device.Get());
    TestEntryFailures(w.device.Get());
    TestUnobservedFor(w.device.Get());
    RunExitTests(w.device.Get());
    RunOrderTests(w.device.Get());
    Check(!coretest::HasDebugErrors(w.device.Get()), "no D3D12 debug-layer errors");
    if (failures == 0) std::printf("menu host queue: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
