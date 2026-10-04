#pragma once
// Proof that menu passes finished before the core may free the model they use (Task 8 fix rounds 1-2, Codex C1-C3).
// The core's retirement gates are keyed on ReShade's proxy device while the private queue is registered natively, so
// menu mode proves completion itself, on the CPU, bounded, off the present path, at the points where the core may free
// the watched feature's model: every host evaluate of it (a re-creation happens only inside one) and its release.
//   - MenuOutstandingPass: a pass the game's evaluates cannot be ordered after. Recover(): after a failed f2 Signal a
//     fresh fence signalled on the private queue orders after the pass; if that fails too, nothing can prove the pass
//     finished (unfenced: withheld for the rest of the session). Hold(): a pass whose Signal succeeded but that did
//     not complete in time. ProofFor() picks what must complete before a model call (under the pipeline's lock),
//     MenuProvePass() waits for it (without the lock), Settle() applies the result (under the lock).
//     Final review (Claude I2): a pass still running after the bound on a live device is a slow GPU, not a broken one:
//     Settle answers Slow (withhold this and the following evaluates without waiting until it completed, menu mode stays
//     on). Only a removed device (a fence at UINT64_MAX, menu_fence.h) answers Quarantine.
//   - MenuReleaseTombstone: the watched feature's release is under way; its evaluates are withheld until it ended.
// Not thread-safe unless stated: the pipeline's lock covers MenuOutstandingPass.
#include "hosts/reshade/menu_fence.h"
#include <d3d12.h>
#include <wrl/client.h>
#include <atomic>

namespace ofps::reshade {

struct MenuPassProof {
    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    UINT64 value = 0;
    bool unfenced = false;    // nothing can prove it: never proven
    bool wait = false;        // a bounded CPU wait is allowed (not after an earlier wait for this pass timed out)
    bool outstanding = false; // the fence is the outstanding pass's (Settle lifts it once it completed)
    bool Needed() const { return unfenced || fence != nullptr; }
};

// NotDone: not complete, and no wait was allowed. Removed: the fence's device was removed (nothing can prove it).
enum class MenuPassWait { Done, Waited, TimedOut, NotDone, Unfenced, Removed };
// Resumed: an outstanding pass completed. Slow: the first timeout on a live device (withheld, menu mode stays on).
// Quarantine: the device was removed (menu mode off for the session, every object kept).
enum class MenuPassGate { Call, Resumed, Withhold, Slow, Quarantine };

// Without a lock: Done when already complete; else, if allowed, waits at most `ms` (the event is closed unless an armed
// wait timed out, when the fence may still set it). A removed device's fence never counts as complete: Removed.
MenuPassWait MenuProvePass(const MenuPassProof &proof, DWORD ms);
inline bool MenuPassProven(MenuPassWait result) { return result == MenuPassWait::Done || result == MenuPassWait::Waited; }

class MenuOutstandingPass {
public:
    // The last private pass whose f2 Signal succeeded (fix round 3): its own record, kept whatever happens to the GPU
    // objects afterwards (a later f3 failure poisons them, a swap chain's drain detaches them); the fence stays alive.
    void NotePass(ID3D12Fence *f2, UINT64 value);
    bool Recover(ID3D12Device *device, ID3D12CommandQueue *privateQueue); // false: unfenced (fail closed)
    void Hold(ID3D12Fence *fence, UINT64 value);
    // What must complete before the watched feature's model may be called or released: the outstanding pass, else the
    // last noted pass while it has not completed. forceWait: a release waits again even after an earlier wait for the
    // outstanding pass timed out.
    MenuPassProof ProofFor(bool forceWait = false) const;
    // TimedOut / Removed: held and expired (no more waits); Slow / Quarantine.
    MenuPassGate Settle(const MenuPassProof &proof, MenuPassWait result);
    bool Pending() const { return unfenced_ || fence_ != nullptr; }
    bool Unfenced() const { return unfenced_; }
    bool Expired() const { return expired_; }
    ID3D12Fence *LastFence() const { return last_.Get(); } // the last noted pass (null: none since the last Clear)
    UINT64 LastValue() const { return lastValue_; }
    // Final review M4 (a present's poll): the last noted pass passed on a live device, so nothing waits for it any more.
    void ForgetCompletedLast();
    void Clear(); // the feature was released (after its release took its proof)

private:
    Microsoft::WRL::ComPtr<ID3D12Fence> fence_, last_;
    UINT64 value_ = 0, lastValue_ = 0;
    bool unfenced_ = false, expired_ = false;
};

// Host evaluates of the watched feature between their proof and the end of the core's evaluate (fix round 3): while one
// is held the present path submits no menu pass (it skips, never waits), so "proof -> core call" stays atomic against a
// menu re-entry. Under the pipeline's lock; every Take is matched by one Give (RAII in MenuHostEvaluate).
class MenuEvaluateLeases {
public:
    void Take() { ++held_; }
    void Give() { if (held_) --held_; }
    bool Held() const { return held_ != 0; }

private:
    unsigned held_ = 0;
};

// Thread-safe (atomic): set under the pipeline's lock when the watched feature's release starts, cleared when HookRelease
// has finished; an evaluate of that handle meanwhile is withheld.
class MenuReleaseTombstone {
public:
    void Set(void *handle) { handle_.store(handle); }
    bool Blocks(void *handle) const { return handle && handle_.load() == handle; }
    void Clear(void *handle) { handle_.compare_exchange_strong(handle, nullptr); }

private:
    std::atomic<void *> handle_{nullptr};
};

} // namespace ofps::reshade
