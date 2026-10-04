// Menu mode's proof that a pass finished (menu_outstanding.h, Task 8 fix rounds 1-2) on WARP: a host evaluate with an
// unfinished pass waits (bounded) and proceeds, a timeout withholds (a slow pass; only a removed device quarantines),
// later evaluates do not wait again, an unfenced pass withholds for the session, a release is allowed only for a proven
// pass, a completed last pass is forgotten, and the release tombstone.
#include "test_menu_host_queue_support.h"
#include "test_menu_host_queue_fakes.h"
#include "hosts/reshade/menu_outstanding.h"
#include <thread>

using namespace ofps::reshade;
using namespace menutest;
namespace {
constexpr auto kDirect = D3D12_COMMAND_LIST_TYPE_DIRECT;

// A menu pass still running on the private queue: held behind `gate`, then it signals f2 = 1.
struct HeldPass {
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Fence> gate, f2;
    bool Start(ID3D12Device *device) {
        queue = Queue(device, kDirect);
        gate = Fence(device);
        f2 = Fence(device);
        return queue && gate && f2 && SUCCEEDED(queue->Wait(gate.Get(), 1)) && SUCCEEDED(queue->Signal(f2.Get(), 1));
    }
};

// A fence whose SetEventOnCompletion fails and which never completes (fix round 2, minor D). Stack object.
class RefusingFence final : public ID3D12Fence {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void **out) override { if (out) *out = nullptr; return E_NOINTERFACE; }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT *, void *) override { return E_FAIL; }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void *) override { return E_FAIL; }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown *) override { return E_FAIL; }
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR) override { return E_FAIL; }
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID, void **device) override { if (device) *device = nullptr; return E_FAIL; }
    UINT64 STDMETHODCALLTYPE GetCompletedValue() override { return 0; }
    HRESULT STDMETHODCALLTYPE SetEventOnCompletion(UINT64, HANDLE) override { return E_FAIL; }
    HRESULT STDMETHODCALLTYPE Signal(UINT64) override { return E_FAIL; }
};

DWORD Handles() { DWORD n = 0; GetProcessHandleCount(GetCurrentProcess(), &n); return n; }

// Ruling A: a host evaluate with an unfinished last pass waits for it and proceeds.
void TestEvaluateWaits(ID3D12Device *device) {
    MenuOutstandingPass p;
    Check(!p.ProofFor().Needed(), "no pass: nothing to prove");
    auto done = Fence(device);
    done->Signal(3);
    p.NotePass(done.Get(), 3);
    Check(!p.ProofFor().Needed(), "a completed last pass: nothing to prove (no wait)");
    HeldPass pass;
    Check(pass.Start(device), "a pass is held on the GPU");
    p.NotePass(pass.f2.Get(), 1);
    const MenuPassProof proof = p.ProofFor();
    Check(proof.Needed() && proof.wait && !proof.outstanding, "an unfinished last pass must be proven, with a wait");
    std::thread opener([&] { Sleep(30); pass.gate->Signal(1); });
    const MenuPassWait result = MenuProvePass(proof, 5000);
    opener.join();
    Check(result == MenuPassWait::Waited && MenuPassProven(result), "the evaluate waits until the pass completed");
    Check(p.Settle(proof, result) == MenuPassGate::Call && !p.Pending(), "then the model is called");
}

// Ruling A timeout on a live device (final review, Claude I2: a slow GPU is not quarantined): withheld, menu mode stays
// on; later evaluates withhold without waiting; a release waits again; resumed once the pass completed.
void TestEvaluateTimeout(ID3D12Device *device) {
    MenuOutstandingPass p;
    HeldPass pass;
    Check(pass.Start(device), "timeout: a pass is held on the GPU");
    p.NotePass(pass.f2.Get(), 1);
    const MenuPassProof proof = p.ProofFor();
    const MenuPassWait result = MenuProvePass(proof, 30);
    Check(result == MenuPassWait::TimedOut && !MenuPassProven(result), "timeout: the bounded wait times out");
    Check(p.Settle(proof, result) == MenuPassGate::Slow && p.Pending() && p.Expired(), "timeout: withheld as a slow pass, not quarantined");
    const MenuPassProof later = p.ProofFor();
    Check(later.Needed() && !later.wait && later.outstanding, "timeout: a later evaluate proves the same pass without waiting");
    const MenuPassWait again = MenuProvePass(later, 5000);
    Check(again == MenuPassWait::NotDone && p.Settle(later, again) == MenuPassGate::Withhold, "timeout: and is withheld at once");
    Check(p.ProofFor(true).wait, "timeout: a release waits again");
    Check(SUCCEEDED(pass.gate->Signal(1)) && Reaches(pass.f2.Get(), 1, 5000), "timeout: the pass completes later");
    const MenuPassProof last = p.ProofFor();
    const MenuPassWait now = MenuProvePass(last, 0);
    Check(now == MenuPassWait::Done && p.Settle(last, now) == MenuPassGate::Resumed && !p.Pending(), "timeout: resumed once it completed");
}

// A real f2 Signal failure after the pass was submitted: the fresh fence on the private queue passes only after the pass.
void TestRecover(ID3D12Device *device) {
    HeldPass pass;
    Check(pass.Start(device), "recover: a pass is held on the GPU");
    MenuOutstandingPass p;
    Check(p.Recover(device, pass.queue.Get()) && p.Pending() && !p.Unfenced(), "recover: a fresh fence is signalled after the pass");
    const MenuPassProof proof = p.ProofFor();
    Check(proof.outstanding && proof.wait && MenuProvePass(proof, 30) == MenuPassWait::TimedOut, "recover: not proven while the pass runs");
    Check(SUCCEEDED(pass.gate->Signal(1)), "recover: the pass completes");
    const MenuPassProof after = p.ProofFor(true);
    const MenuPassWait result = MenuProvePass(after, 5000);
    Check(MenuPassProven(result) && p.Settle(after, result) == MenuPassGate::Resumed, "recover: proven once it completed");
}

// The recovery Signal fails too: nothing proves the pass; withheld for the session, a release is never allowed.
void TestUnfenced(ID3D12Device *device) {
    auto real = Queue(device, kDirect);
    FailingQueue failing(real.Get(), true, false);
    MenuOutstandingPass p;
    Check(!p.Recover(device, &failing) && failing.signals == 1 && p.Unfenced(), "unfenced: the recovery Signal failed");
    auto f2 = Fence(device);
    f2->Signal(1);
    p.Hold(f2.Get(), 1);
    for (int i = 0; i < 3; ++i) {
        const MenuPassProof proof = p.ProofFor(true);
        const MenuPassWait result = MenuProvePass(proof, 5000);
        Check(result == MenuPassWait::Unfenced && !MenuPassProven(result) && p.Settle(proof, result) == MenuPassGate::Withhold,
              "unfenced: withheld on every evaluate, a release not allowed");
    }
    p.Clear();
    Check(!p.Pending() && !p.ProofFor().Needed(), "Clear (the feature was released) ends it");
}

// Ruling B: a release is allowed only for a proven pass (ngx_hook_api.cpp skips the core's release otherwise).
void TestReleaseDecision(ID3D12Device *device) {
    HeldPass pass;
    Check(pass.Start(device), "release: a pass is held on the GPU");
    MenuOutstandingPass p;
    p.NotePass(pass.f2.Get(), 1);
    const MenuPassWait timedOut = MenuProvePass(p.ProofFor(true), 30);
    Check(timedOut == MenuPassWait::TimedOut && !MenuPassProven(timedOut), "release: a timed-out wait does not allow the release");
    Check(SUCCEEDED(pass.gate->Signal(1)) && Reaches(pass.f2.Get(), 1, 5000), "release: the pass completes");
    Check(MenuPassProven(MenuProvePass(p.ProofFor(true), 500)), "release: a completed pass allows it");
    Check(!MenuPassProven(MenuPassWait::NotDone) && !MenuPassProven(MenuPassWait::Unfenced), "release: never without a proof");
}

// Minor D: an event whose SetEventOnCompletion failed is closed; only an armed wait that timed out keeps its event.
void TestEvents(ID3D12Device *device) {
    RefusingFence refusing;
    MenuPassProof proof;
    proof.fence = &refusing;
    proof.value = 1;
    proof.wait = true;
    const DWORD before = Handles();
    Check(MenuProvePass(proof, 30) == MenuPassWait::TimedOut && Handles() == before, "events: an unarmed event is closed");
    HeldPass pass;
    Check(pass.Start(device), "events: a pass is held on the GPU");
    MenuPassProof armed;
    armed.fence = pass.f2;
    armed.value = 1;
    armed.wait = true;
    const DWORD beforeArmed = Handles();
    Check(MenuProvePass(armed, 20) == MenuPassWait::TimedOut && Handles() == beforeArmed + 1, "events: an armed, timed-out event is kept");
    pass.gate->Signal(1);
    Check(Reaches(pass.f2.Get(), 1, 5000), "events: the pass completes");
    const DWORD beforeWait = Handles();
    Check(MenuProvePass(armed, 20) == MenuPassWait::Done && Handles() == beforeWait, "events: a completed pass opens no event");
}

// Fix round 3 (1, 2): the last fenced pass is its own record. It survives the GPU objects (a poisoned Gpu after an f3
// failure, a Gpu drained and detached at destroy_swapchain: the pipeline drops its references, the record keeps the
// fence) and an outstanding recovery fence that completed first.
void TestRecordSurvives(ID3D12Device *device) {
    HeldPass pass;
    Check(pass.Start(device), "record: a pass is held on the GPU");
    MenuOutstandingPass p;
    p.NotePass(pass.f2.Get(), 1);
    ID3D12Fence *const f2 = pass.f2.Get();
    f2->AddRef();
    pass.f2.Reset(); // the Gpu (its f2 reference) is gone
    const MenuPassProof proof = p.ProofFor();
    Check(proof.Needed() && proof.wait && proof.fence.Get() == f2 && !proof.outstanding, "record: still proven after the Gpu went");
    Check(MenuProvePass(proof, 20) == MenuPassWait::TimedOut, "record: not proven while the pass runs");
    auto idle = Queue(device, kDirect);
    Check(p.Recover(device, idle.Get()), "record: a recovery fence on an idle queue");
    const MenuPassProof recovery = p.ProofFor(true);
    const MenuPassWait recovered = MenuProvePass(recovery, 5000);
    Check(recovery.outstanding && MenuPassProven(recovered) && p.Settle(recovery, recovered) == MenuPassGate::Resumed,
          "record: the recovery fence completed first");
    Check(p.ProofFor().Needed() && p.ProofFor().fence.Get() == f2, "record: the last pass is still required afterwards");
    Check(SUCCEEDED(pass.gate->Signal(1)) && Reaches(f2, 1, 5000), "record: the pass completes");
    Check(!p.ProofFor().Needed(), "record: then nothing is left to prove");
    f2->Release();
}

// Final review (Codex C1, Claude I2): a removed device's UINT64_MAX never proves a pass; that alone quarantines. A
// release is not allowed either.
void TestRemoved() {
    RemovedFence removed;
    Check(MenuFenceRemoved(&removed) && !MenuFencePassed(&removed, 1) && MenuFenceProgress(&removed) == 0,
          "removed: UINT64_MAX is a removal, never a completion, no progress");
    MenuOutstandingPass p;
    p.NotePass(&removed, 5);
    const MenuPassProof proof = p.ProofFor();
    Check(proof.Needed() && proof.wait, "removed: the last pass still needs its proof");
    const MenuPassWait result = MenuProvePass(proof, 30);
    Check(result == MenuPassWait::Removed && !MenuPassProven(result), "removed: the proof reports the removal, without a wait");
    Check(p.Settle(proof, result) == MenuPassGate::Quarantine && p.Pending(), "removed: quarantined, the pass stays outstanding");
    const MenuPassProof later = p.ProofFor(true);
    Check(MenuProvePass(later, 30) == MenuPassWait::Removed, "removed: a release is never allowed");
    p.ForgetCompletedLast();
    Check(p.LastFence() == &removed, "removed: the last pass is never forgotten as completed");
    p.Clear();
}

// Final review M4: a present that sees the last pass completed forgets it, so a later device removal cannot make an old
// menu's pass look unfinished.
void TestForgetCompleted(ID3D12Device *device) {
    MenuOutstandingPass p;
    HeldPass pass;
    Check(pass.Start(device), "forget: a pass is held on the GPU");
    p.NotePass(pass.f2.Get(), 1);
    p.ForgetCompletedLast();
    Check(p.LastFence() == pass.f2.Get(), "forget: a running pass is kept");
    Check(SUCCEEDED(pass.gate->Signal(1)) && Reaches(pass.f2.Get(), 1, 5000), "forget: the pass completes");
    p.ForgetCompletedLast();
    Check(p.LastFence() == nullptr && !p.ProofFor().Needed(), "forget: a completed pass is dropped");
}

// Fix round 3 (3): the evaluate lease blocks menu passes from before the proof until the core's evaluate returned.
void TestLeases() {
    MenuEvaluateLeases leases;
    Check(!leases.Held(), "leases: none by default");
    leases.Take();
    leases.Take();
    leases.Give();
    Check(leases.Held(), "leases: held while any evaluate holds one");
    leases.Give();
    Check(!leases.Held(), "leases: released by the last evaluate");
    leases.Give();
    Check(!leases.Held(), "leases: an extra Give is harmless");
}

// Ruling C: the watched feature's release is under way: evaluates of that handle are withheld until it ended.
void TestTombstone() {
    MenuReleaseTombstone t;
    int a = 0, b = 0;
    Check(!t.Blocks(&a) && !t.Blocks(nullptr), "tombstone: nothing blocked by default");
    t.Set(&a);
    Check(t.Blocks(&a) && !t.Blocks(&b) && !t.Blocks(nullptr), "tombstone: only the released handle is withheld");
    t.Clear(&b);
    Check(t.Blocks(&a), "tombstone: another handle's release does not clear it");
    t.Clear(&a);
    Check(!t.Blocks(&a), "tombstone: cleared at the end of HookRelease");
}
} // namespace

int main() {
    coretest::WarpDevice w;
    if (!coretest::CreateWarpDevice(w)) { std::fprintf(stderr, "FAIL: WARP device\n"); return 1; }
    TestEvaluateWaits(w.device.Get());
    TestEvaluateTimeout(w.device.Get());
    TestRecover(w.device.Get());
    TestUnfenced(w.device.Get());
    TestReleaseDecision(w.device.Get());
    TestEvents(w.device.Get());
    TestTombstone();
    TestRecordSurvives(w.device.Get());
    TestLeases();
    TestRemoved();
    TestForgetCompleted(w.device.Get());
    Check(!coretest::HasDebugErrors(w.device.Get()), "no D3D12 debug-layer errors");
    if (failures == 0) std::printf("menu outstanding pass: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
