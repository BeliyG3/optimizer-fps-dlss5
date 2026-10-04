#pragma once
// A hook on ID3D12CommandQueue::ExecuteCommandLists for hosts whose D3D12 device ReShade never wraps (OptiScaler's own
// device in D3D11 games: Fallen Order with wilsjo2 v0.8.3). ReShade's execute_command_list then never reports the queue
// that runs the host's NR list, so menu mode waits for that queue forever (menu_host_queue.h) and the core never learns
// it. The hook reports those submissions itself (addon/queue_events.cpp holds the sinks).
//
// - A vtable entry swap, no instruction patching (fix round 1, I1): the ExecuteCommandLists slot of a temporary queue's
//   vtable on the host list's device gets the hook's address with one aligned atomic pointer write (VirtualProtect
//   around it); the slot's previous pointer is kept and called. Every queue of that class shares the vtable. A vtable
//   that does not lie in a d3d12*.dll (a ReShade proxy, another wrapper) is refused. Installed once, lazily, from a host
//   evaluate (off the present path).
// - Two sinks around the native call (fix round 1, C1): `before` gets what must precede the list on its queue (menu
//   mode's exit Wait, which the native call must follow); `after` gets completion tracking (submission serials, guide
//   fences, queue registration) only once the native call returned, so no fence that marks the list complete can be
//   enqueued ahead of it. Calls made from a skipped module (the caller's return address) reach neither: the add-on and
//   the core report their own submissions, and ReShade's proxies fire its event. No lock is taken in the hook itself;
//   the sinks take only leaf locks, never block and never call back into ReShade.
// - The module holding the hook is pinned once the swap is in (fix round 1, C2): it is never unmapped while the slot can
//   point at it. Restore (DLL_PROCESS_DETACH, i.e. process exit once pinned) writes the original pointer back unless
//   someone chained after us.
// Install, skip and restore come from one thread at a time.
#include <d3d12.h>

namespace ofps::reshade {

using SubmitSink = void (*)(ID3D12CommandQueue *queue, UINT count, ID3D12CommandList *const *lists);
struct SubmitSinks {
    SubmitSink before = nullptr; // before the native call
    SubmitSink after = nullptr;  // after the native call returned
};

// True when installed (now or before). False: *why says why (static text); nothing was changed.
bool SubmitHookInstall(ID3D12Device *device, const SubmitSinks &sinks, const HMODULE *skipped, unsigned skippedCount,
                       const char **why);
bool SubmitHookInstalled();
// True when `queue` uses the swapped vtable: its submissions reach the hook.
bool SubmitHookCovers(ID3D12CommandQueue *queue);
// The modules whose own calls are not reported (null entries ignored; at most 4). Replaces the set given at install.
void SubmitHookSkip(const HMODULE *skipped, unsigned skippedCount);
void SubmitHookSilence(); // the hook only forwards from now on
// Silences, then writes the original pointer back. False: another hook replaced ours in the slot since (left as is).
bool SubmitHookRestore();

} // namespace ofps::reshade
