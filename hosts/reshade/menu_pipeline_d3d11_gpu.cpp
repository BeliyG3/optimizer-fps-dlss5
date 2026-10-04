#include "hosts/reshade/menu_pipeline_state.h"
#include "hosts/reshade/menu_bridge_d3d11.h"
#include "hosts/reshade/menu_dump.h"
#include "hosts/reshade/shell_host.h"

// The D3D11 bridge's lifetime towards the core (Task 10, correction C1, as menu_pipeline_gpu.cpp on D3D12): the private
// queue is registered before its first submission, each pass's list is reported (menu_pipeline_d3d11.cpp), and the queue
// is unregistered only once the bridge's fence passed everything (a completed drain, or a late bridge whose fence
// passed later). A bridge that is poisoned, quarantined or on a removed device keeps its queue registered and its objects
// for the session.
//
// Keying (fix round 1, review I1 / Codex C). The private queue is made on the host list's device, so the core tracks it
// under the key the host's NR recordings carry. Submission::Untrack cancels a key's unsubmitted recordings when its last
// queue goes (Task 8 fix round 1), so the unregister is safe only while another queue of that key stays tracked:
//   - native case (the D3D11 bench, 0c.md): the host list answers the native device, which differs from the NR colour's
//     (ReShade's wrapped device), and ReShade did not report our queue. ReShade registers the host bridge's own queue
//     under that native device, so ours is never the key's last queue: registered, reported per pass, unregistered at
//     the drain as on D3D12;
//   - wrapped case (the host list's device equals the NR colour's, or a queue event arrived while ours was created):
//     our queue may be the only one of the host recordings' key. Fail closed: it stays registered and alive for the
//     session (no unregister, a reference kept at the drain).
// In both cases the queue is registered before its first ExecuteCommandLists and the pipeline reports every pass on it
// itself (fix round 2); if ReShade also reports it natively, the duplicate is harmless (menu_pipeline_d3d11.cpp AfterPass).
// Under the pipeline's lock.
namespace ofps::reshade::menu_detail {
namespace {
void ReleaseDrained(MenuBridgeD3D11 *b) { // everything the bridge's fence covered has passed
    if (b->Registered() && b->KeepRegistered()) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            Log("menu pipeline: the bridge's private queue %p is kept registered: the bridge records NR on the wrapped device (for the session)",
                (void *) b->Queue());
        }
        b->Queue()->AddRef(); // alive for the session, as the core's registration
    } else if (b->Registered()) {
        if (IOfpsCore *core = Core()) core->UnregisterQueue(b->Queue());
    }
    b->MarkRegistered(false, false);
    MenuDump11Release();
    MenuGpuTimeRelease();
    MenuModelReleaseDevice();
    delete b;
}
} // namespace

// Called by the pipeline's Open right after MenuBridgeD3D11::Open, before Setup's warm-up: the queue's first
// ExecuteCommandLists, in both cases (fix round 2). Keyed on the device the queue was created on (the host list's).
void RegisterBridgeD3D11(Pipeline &p, MenuBridgeD3D11 &b) {
    IOfpsCore *core = Core();
    if (!core || !b.Queue() || !b.Device()) return; // no core: nothing to report to
    const bool wrapped = p.listOnWrapped || b.QueueReported();
    core->RegisterQueue(b.Device(), b.Queue()); // void under ABI 1: "registered" means handed to the core
    b.MarkRegistered(true, wrapped);
    Log("menu pipeline: the bridge's private queue %p handed to the core under device %p: %s", (void *) b.Queue(), (void *) b.Device(),
        !wrapped ? "native case (the host list's device differs from the NR colour's, no queue event during its creation); unregistered once drained"
                 : b.QueueReported() ? "wrapped case (a queue event arrived during its creation); kept registered for the session"
                                     : "wrapped case (the host list's device is the NR colour's); kept registered for the session");
}

// Without the pipeline's lock (final review M1): until the drain's shared deadline; touches only the bridge.
bool WaitD3D11(MenuBridgeD3D11 &b, double deadlineMs) {
    const double left = deadlineMs - NowMs();
    return b.Drain(left > 0 ? DWORD(left) + 1 : 0);
}

void FinishD3D11(Pipeline &p, MenuBridgeD3D11 *b, bool drained) {
    if (drained) {
        ReleaseDrained(b);
        Log("menu pipeline: the bridge drained (the fence passed) and released");
        return;
    }
    EndSession(p, p.presents, "the GPU did not finish menu work at a swap chain's teardown (see ReShade.log)");
    if (b->Poisoned()) {
        Log("menu pipeline: the bridge has a submission that was never fenced (or was quarantined, or its device was removed); its objects are kept (leaked), menu mode off for the session");
        return;
    }
    p.lateBridges.push_back(b); // kept owned; released on a later present or teardown once the fence passed
    Log("menu pipeline: the bridge's fence did not pass within the 500 ms of the teardown; its objects are kept until it does, menu mode off for the session");
}

void PollLateD3D11(Pipeline &p) {
    std::erase_if(p.lateBridges, [](MenuBridgeD3D11 *b) {
        if (b->Removed()) { // device removed: nothing proves completion (fix round 1: never "passed")
            b->Keep();
            Log("menu pipeline: the device was removed while a drained bridge waited for its fence; its objects are kept (leaked)");
            return true;
        }
        if (!b->Done()) return false;
        b->Retire();
        b->Poll();
        ReleaseDrained(b);
        Log("menu pipeline: a bridge whose drain timed out passed its fence later and is released now");
        return true;
    });
}

void QuarantineD3D11(Pipeline &p) { p.bridge->Keep(); }

} // namespace ofps::reshade::menu_detail
