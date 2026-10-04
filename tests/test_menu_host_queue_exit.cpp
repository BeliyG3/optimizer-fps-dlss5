// Menu mode's exit ordering against a host NR queue (menu_host_queue.h), on WARP: OrderExitNow in the hooked host
// evaluate (GPU Wait, bounded CPU fallback, quarantine) and ListExecuting's safety net. Fix round 1 (Codex I1).
#include "test_menu_host_queue_support.h"
#include "test_menu_host_queue_fakes.h"
#include "hosts/reshade/menu_host_queue.h"
#include <thread>

using namespace ofps::reshade;
namespace menutest {
namespace {
constexpr auto kDirect = D3D12_COMMAND_LIST_TYPE_DIRECT;
constexpr auto kCompute = D3D12_COMMAND_LIST_TYPE_COMPUTE;

// The last menu pass completes `ms` later, from another thread (the private queue's Signal of f2).
std::thread CompletesAfter(ID3D12Fence *f2, UINT64 value, int ms) {
    return std::thread([f2, value, ms] { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); f2->Signal(value); });
}

// The exit evaluate enqueues the host queue's Wait before its model call; the exit list runs behind it.
void TestExitGpuWait(ID3D12Device *device) {
    auto present = Queue(device, kDirect), real = Queue(device, kCompute);
    FailingQueue host(real.Get(), false, false); // record only
    auto f2 = Fence(device), check = Fence(device);
    ComPtr<ID3D12CommandAllocator> allocator, exitAllocator;
    auto list = List(device, kCompute, allocator), exitList = List(device, kCompute, exitAllocator);
    auto progress = Fence(device);
    MenuHostQueue q;
    q.ListRecorded(list.Get());
    q.ListExecuting(&host, list.Get());
    real->Signal(progress.Get(), 1); // the host queue's progress up to the exit Wait
    Check(q.OrderExitNow(exitList.Get(), f2.Get(), 1, present.Get()) == MenuExitOrder::GpuWaitQueued, "the exit Wait is queued");
    Check(host.waits == 1 && host.waitFence == f2.Get() && host.waitValue == 1, "on the host queue, for the last pass");
    Check(q.ListExecuting(&host, exitList.Get()) == MenuExitOrder::NotNeeded, "the exit list runs on that queue: no CPU wait");
    Check(q.ListExecuting(&host, exitList.Get()) == MenuExitOrder::NotNeeded && host.waits == 1, "the Wait is enqueued once");
    real->Signal(check.Get(), 1);
    Check(Reaches(progress.Get(), 1, 5000) && check->GetCompletedValue() == 0, "the host queue holds behind the last pass");
    f2->Signal(1);
    Check(Reaches(check.Get(), 1, 2000), "and runs once it passed");
    Check(q.OrderExitNow(exitList.Get(), f2.Get(), 1, present.Get()) == MenuExitOrder::NotNeeded && host.waits == 1,
          "a last pass that already completed needs nothing");
}

// No GPU Wait possible: host queue unknown, the Wait fails, or the exit list cannot be tagged -> bounded CPU wait.
void TestExitCpuFallback(ID3D12Device *device) {
    auto present = Queue(device, kDirect), real = Queue(device, kCompute);
    ComPtr<ID3D12CommandAllocator> allocator;
    auto list = List(device, kCompute, allocator);
    {
        auto f2 = Fence(device);
        MenuHostQueue q; // no host queue observed
        auto pass = CompletesAfter(f2.Get(), 1, 20);
        Check(q.OrderExitNow(list.Get(), f2.Get(), 1, present.Get()) == MenuExitOrder::CpuWaited, "unknown host queue: CPU wait");
        pass.join();
    }
    {
        auto f2 = Fence(device);
        FailingQueue host(real.Get(), false, true);
        MenuHostQueue q;
        q.ListRecorded(list.Get());
        q.ListExecuting(&host, list.Get());
        auto pass = CompletesAfter(f2.Get(), 1, 20);
        Check(q.OrderExitNow(list.Get(), f2.Get(), 1, present.Get()) == MenuExitOrder::CpuWaited && host.waits == 1,
              "a failed exit Wait falls back to the CPU wait");
        pass.join();
        Check(q.ListExecuting(&host, list.Get()) == MenuExitOrder::NotNeeded && host.waits == 1, "and arms no safety net");
    }
    {
        auto f2 = Fence(device);
        FailingQueue host(real.Get(), false, false);
        menutest::UntaggableList untaggable;
        MenuHostQueue q;
        q.ListRecorded(list.Get());
        q.ListExecuting(&host, list.Get());
        auto pass = CompletesAfter(f2.Get(), 1, 20);
        Check(q.OrderExitNow(&untaggable, f2.Get(), 1, present.Get()) == MenuExitOrder::CpuWaited && host.waits == 0,
              "an untaggable exit list is waited for on the CPU (its queue cannot be checked)");
        pass.join();
    }
}

// The last pass never completes: at most 100 ms, whatever the caller asks, then quarantine.
void TestExitQuarantine(ID3D12Device *device) {
    auto present = Queue(device, kDirect), other = Queue(device, kCompute);
    auto f2 = Fence(device);
    ComPtr<ID3D12CommandAllocator> allocator;
    auto list = List(device, kCompute, allocator);
    MenuHostQueue q;
    const auto t0 = std::chrono::steady_clock::now();
    Check(q.OrderExitNow(list.Get(), f2.Get(), 1, present.Get(), 5000) == MenuExitOrder::Quarantine, "a pass that never completes quarantines");
    const double ms = Ms(t0);
    Check(ms >= 90.0 && ms < 1000.0, "the fallback wait is bounded to 100 ms");
    Check(q.ListExecuting(other.Get(), list.Get()) == MenuExitOrder::NotNeeded, "no safety net after a CPU decision");
}

// The exit list runs on another queue than the one waited on: the safety net waits on the CPU before the native call.
// Fix round 2: first a GPU Wait on the actual queue (before the native submit that follows the event), the bounded CPU
// wait only when that Wait fails, quarantine when that times out.
void TestExitSafetyNet(ID3D12Device *device) {
    auto present = Queue(device, kDirect), x = Queue(device, kCompute), realY = Queue(device, kCompute);
    FailingQueue y(realY.Get(), false, false), badY(realY.Get(), false, true);
    auto f2 = Fence(device), f3 = Fence(device), f4 = Fence(device);
    ComPtr<ID3D12CommandAllocator> allocator, exitAllocator;
    auto list = List(device, kCompute, allocator), exitList = List(device, kCompute, exitAllocator);
    MenuHostQueue q;
    q.ListRecorded(list.Get());
    q.ListExecuting(x.Get(), list.Get());
    Check(q.OrderExitNow(exitList.Get(), f2.Get(), 1, present.Get()) == MenuExitOrder::GpuWaitQueued, "ordered on queue X");
    Check(q.ListExecuting(&y, exitList.Get()) == MenuExitOrder::GpuWaitQueued && y.waits == 1 && y.waitFence == f2.Get() &&
              y.waitValue == 1,
          "the exit list on queue Y gets its own GPU Wait for the last pass");
    f2->Signal(1);
    Check(Free(device, realY.Get(), 2000), "queue Y runs once the pass completed");
    Check(q.OrderExitNow(exitList.Get(), f3.Get(), 1, present.Get()) == MenuExitOrder::GpuWaitQueued, "a later exit on X");
    auto pass = CompletesAfter(f3.Get(), 1, 20);
    Check(q.ListExecuting(&badY, exitList.Get()) == MenuExitOrder::CpuWaited && badY.waits == 1,
          "Y's Wait fails: the bounded CPU wait covers it");
    pass.join();
    Check(q.OrderExitNow(exitList.Get(), f4.Get(), 1, present.Get()) == MenuExitOrder::GpuWaitQueued, "another exit on X");
    const auto t0 = std::chrono::steady_clock::now();
    Check(q.ListExecuting(&badY, exitList.Get()) == MenuExitOrder::Quarantine && Ms(t0) < 1000.0,
          "Y's Wait fails and the pass never completes: quarantine, bounded");
    f4->Signal(1); // release queue X's Wait
    Check(Free(device, x.Get(), 2000), "queue X runs on");
}

// Fix round 2: OrderExitNow trusts the learned queue only when every recording before the exit one was seen executing
// on that queue (since the previous exit); otherwise it waits on the CPU before the model call and enqueues no Wait.
void TestExitNeedsObservedHistory(ID3D12Device *device) {
    auto present = Queue(device, kDirect), realA = Queue(device, kCompute), realB = Queue(device, kCompute);
    ComPtr<ID3D12CommandAllocator> a1, a2, a3;
    auto l1 = List(device, kCompute, a1), l2 = List(device, kCompute, a2), exitList = List(device, kCompute, a3);
    auto exitOn = [&](MenuHostQueue &q) {
        auto f2 = Fence(device);
        auto pass = CompletesAfter(f2.Get(), 1, 20);
        const MenuExitOrder order = q.OrderExitNow(exitList.Get(), f2.Get(), 1, present.Get());
        pass.join();
        return order;
    };
    {
        FailingQueue a(realA.Get(), false, false);
        MenuHostQueue q;
        q.ListRecorded(l1.Get());
        q.ListExecuting(&a, l1.Get());
        q.ListRecorded(l2.Get()); // not seen executing yet
        Check(exitOn(q) == MenuExitOrder::CpuWaited && a.waits == 0, "latest recording unobserved: CPU wait, no Wait on the older queue");
    }
    {
        FailingQueue a(realA.Get(), false, false);
        MenuHostQueue q;
        q.ListRecorded(l1.Get()); // never seen executing
        q.ListRecorded(l2.Get());
        q.ListExecuting(&a, l2.Get());
        Check(exitOn(q) == MenuExitOrder::CpuWaited && a.waits == 0, "an earlier recording never seen executing: CPU wait");
    }
    {
        FailingQueue a(realA.Get(), false, false), b(realB.Get(), false, false);
        MenuHostQueue q;
        q.ListRecorded(l1.Get());
        q.ListExecuting(&a, l1.Get());
        q.ListRecorded(l2.Get());
        q.ListExecuting(&b, l2.Get());
        Check(exitOn(q) == MenuExitOrder::CpuWaited && a.waits == 0 && b.waits == 0, "recordings on two queues: CPU wait");
        q.ListRecorded(l1.Get());
        q.ListExecuting(&b, l1.Get());
        auto f2 = Fence(device);
        Check(q.OrderExitNow(exitList.Get(), f2.Get(), 1, present.Get()) == MenuExitOrder::GpuWaitQueued && b.waits == 1,
              "the history restarts after an exit: one queue again, a GPU Wait");
        f2->Signal(1);
    }
    {
        FailingQueue a(realA.Get(), false, false);
        UntaggableList untaggable;
        MenuHostQueue q;
        q.ListRecorded(l1.Get());
        q.ListExecuting(&a, l1.Get());
        q.ListRecorded(&untaggable);
        Check(exitOn(q) == MenuExitOrder::CpuWaited && a.waits == 0, "an untagged recording: CPU wait");
    }
    Check(Free(device, realA.Get(), 2000) && Free(device, realB.Get(), 2000), "no Wait left on the host queues");
}

// A CPU wait runs outside the object's lock: the queue event on another thread is not held by it.
void TestWaitOutsideLock(ID3D12Device *device) {
    auto present = Queue(device, kDirect), other = Queue(device, kCompute);
    auto f2 = Fence(device);
    ComPtr<ID3D12CommandAllocator> allocator, otherAllocator;
    auto list = List(device, kCompute, allocator), otherList = List(device, kCompute, otherAllocator);
    MenuHostQueue q;
    MenuExitOrder result = MenuExitOrder::NotNeeded;
    std::thread evaluate([&] { result = q.OrderExitNow(list.Get(), f2.Get(), 1, present.Get()); });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const auto t0 = std::chrono::steady_clock::now();
    q.ListExecuting(other.Get(), otherList.Get());
    Check(Ms(t0) < 50.0, "ListExecuting is not held by a running CPU wait");
    evaluate.join();
    Check(result == MenuExitOrder::Quarantine, "the waiting evaluate still reports its own outcome");
}
DWORD Handles() { DWORD n = 0; GetProcessHandleCount(GetCurrentProcess(), &n); return n; }

// Final review (Codex I2): a CPU wait that timed out after its SetEventOnCompletion was armed keeps the event (the fence
// may still set it later; a closed handle could be reused by then); a completed pass opens no event.
void TestExitEventKept(ID3D12Device *device) {
    auto present = Queue(device, kDirect);
    auto f2 = Fence(device);
    ComPtr<ID3D12CommandAllocator> allocator;
    auto list = List(device, kCompute, allocator);
    MenuHostQueue q;
    const DWORD before = Handles();
    Check(q.OrderExitNow(list.Get(), f2.Get(), 1, present.Get(), 20) == MenuExitOrder::Quarantine, "event: the armed wait times out");
    Check(Handles() == before + 1, "event: the armed, timed-out event is kept, never closed");
    f2->Signal(1);
    const DWORD beforeDone = Handles();
    Check(q.OrderExitNow(list.Get(), f2.Get(), 1, present.Get(), 20) == MenuExitOrder::NotNeeded && Handles() == beforeDone,
          "event: a completed pass needs no event");
}

// Final review (Codex I2): a removed device's UINT64_MAX is never a completion: no "nothing to order", and the CPU
// fallback on it quarantines (the exit evaluate is withheld).
void TestExitRemoved(ID3D12Device *device) {
    auto present = Queue(device, kDirect);
    ComPtr<ID3D12CommandAllocator> allocator;
    auto list = List(device, kCompute, allocator);
    RemovedFence removed;
    MenuHostQueue q;
    Check(q.OrderExitNow(list.Get(), &removed, 1, present.Get(), 20) == MenuExitOrder::Quarantine,
          "removed: UINT64_MAX is not a completed last pass");
}
} // namespace

void RunExitTests(ID3D12Device *device) {
    TestExitGpuWait(device);
    TestExitCpuFallback(device);
    TestExitQuarantine(device);
    TestExitSafetyNet(device);
    TestExitNeedsObservedHistory(device);
    TestWaitOutsideLock(device);
    TestExitEventKept(device);
    TestExitRemoved(device);
}

} // namespace menutest
