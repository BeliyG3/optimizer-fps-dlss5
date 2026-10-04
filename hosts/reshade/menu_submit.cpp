#include "hosts/reshade/menu_pipeline_state.h"
#include "hosts/reshade/menu_dump.h"
#include "hosts/reshade/menu_fence.h"
#include "hosts/reshade/menu_guides.h"
#include "hosts/reshade/menu_marker.h"
#include "hosts/reshade/shell_host.h"
#include "hosts/reshade/addon/shell_settings.h"
#include <cstdarg>
#include <cstdio>

// One D3D12 menu present, ordered only by fences:
//   present queue: capture, Signal(f1) -> private queue: Wait(f1), pass, Signal(f2)
//   -> present queue: Wait(f2), write-back (+ dump), Signal(f3).
// A failed Signal/Wait stops menu mode for the session (Stop, menu_pipeline_step.cpp); the frame stays untouched.
namespace ofps::reshade::menu_detail {
namespace {
bool ForcedFailure(MenuFrame &) { return false; } // DebugMenuPass=2

// The private list: the pass between the pipeline's barriers, timed. False: refused, the list must not run. *wrote:
// the pass wrote g.output (MenuFrame::wrote).
bool RecordPass(Gpu &g, PrivateSlot &ps, bool *wrote) {
    if (!Begin(ps.allocator.Get(), ps.list.Get())) return false;
    const unsigned slot = g.privateNext % kRing;
    MenuGpuTimeBegin(ps.list.Get(), slot);
    Barrier(ps.list.Get(), g.capture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Barrier(ps.list.Get(), g.output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    MenuFrame frame{g.capture.Get(), g.output.Get(), ps.list.Get(), g.width, g.height, g.format};
    const MenuPassKind kind = MenuPassSetting();
    const MenuPass pass = kind == MenuPassKind::Marker ? MenuMarkerPass : kind == MenuPassKind::ForcedRefusal ? ForcedFailure : MenuModelPass;
    const bool ok = pass(frame); // the model pass evaluates the snapshot Step bound (MenuModelBind)
    *wrote = frame.wrote;
    Barrier(ps.list.Get(), g.output.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Barrier(ps.list.Get(), g.capture.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    MenuGpuTimeEnd(ps.list.Get(), slot);
    return SUCCEEDED(ps.list->Close()) && ok;
}

// Private queue: Wait(f1), the pass, Signal(f2); true only when the present queue may wait for f2.
bool SubmitPass(Pipeline &p, Gpu &g, PrivateSlot &ps, unsigned long long index) {
    if (FAILED(g.queue->Wait(g.f1.Get(), g.v1))) { MenuGuidesUnused(); Stop(p, index, "private wait on f1"); return false; }
    ID3D12CommandList *lists[] = {ps.list.Get()};
    g.queue->ExecuteCommandLists(1, lists);
    // C1: the core counts this submission on the registered private queue (its retirement gates wait behind it).
    if (g.registered)
        if (IOfpsCore *core = Core()) core->OnCommandListExecuted(g.queue.Get(), ps.list.Get());
    p.state.NotePassSubmitted(); // the model ran on the GPU: the next host evaluate resets NR history, however the run ends
    const unsigned slot = g.privateNext++ % kRing;
    if (!Signal(g.queue.Get(), g.f2.Get(), g.v2)) {
        // C3: the executed list is never fenced. Its objects are never released (poisoned), no further pass runs (menu
        // mode off for the session). Fix round 1 (Codex C2): a recovery Signal on a fresh fence orders after the pass;
        // the watched feature's evaluates are withheld until it passed, or for the session if that Signal failed too.
        g.poisoned = true; ps.value2 = ~0ull;
        const bool fenced = p.outstanding.Recover(p.proxy, g.queue.Get());
        p.loggedWithhold = false;
        Stop(p, index, "private signal of f2 (after the pass was submitted: the feature is quarantined for menu mode)");
        Log("menu pipeline: %s", fenced ? "a recovery fence follows the unfenced pass; the game's NR evaluates wait for it (withheld until it passed)"
                                        : "the recovery signal failed too; the game's NR evaluates of this feature are withheld for the session");
        return false;
    }
    ps.value2 = g.v2;
    p.outstanding.NotePass(g.f2.Get(), g.v2); // the proof's record: kept even if a later f3 fails or the Gpu is drained
    MenuGuidesUsed(g.f2.Get(), g.v2);
    MenuGpuTimeSubmitted(slot, g.v2);
    if (SUCCEEDED(g.presentQueue->Wait(g.f2.Get(), g.v2))) return true;
    Stop(p, index, "present-queue wait on f2");
    return false;
}
} // namespace

} // namespace ofps::reshade::menu_detail

namespace ofps::reshade {
MenuPassKind MenuPassSetting() { return CurrentShellSettings().debugMenuPass; }
bool MenuPassRunsModel() {
    const MenuPassKind kind = MenuPassSetting();
    return kind == MenuPassKind::Model || kind == MenuPassKind::ForcedSignalFailure;
}
} // namespace ofps::reshade

namespace ofps::reshade::menu_detail {

double NowMs() {
    static const double frequency = [] { LARGE_INTEGER f{}; QueryPerformanceFrequency(&f); return double(f.QuadPart); }();
    LARGE_INTEGER t{}; QueryPerformanceCounter(&t);
    return double(t.QuadPart) * 1000.0 / frequency;
}

void Log(const char *fmt, ...) {
    char text[512]; va_list args; va_start(args, fmt); std::vsnprintf(text, sizeof(text), fmt, args); va_end(args);
    Host().Log(OFPS_LOG_INFO, text);
}

void Barrier(ID3D12GraphicsCommandList *list, ID3D12Resource *resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
    list->ResourceBarrier(1, &b);
}

bool Begin(ID3D12CommandAllocator *allocator, ID3D12GraphicsCommandList *list) {
    return SUCCEEDED(allocator->Reset()) && SUCCEEDED(list->Reset(allocator, nullptr));
}

bool Signal(ID3D12CommandQueue *queue, ID3D12Fence *fence, UINT64 &value) {
    if (FAILED(queue->Signal(fence, value + 1))) return false;
    ++value;
    return true;
}

bool SignalNative(Gpu &g, NativeSlot &slot) {
    ++g.nativeNext; // an allocator that may be in flight is never reset again when the signal failed
    if (!Signal(g.presentQueue.Get(), g.f3.Get(), g.v3)) { g.poisoned = true; slot.value3 = ~0ull; return false; }
    slot.value3 = g.v3;
    MenuDumpSubmitted(g.v3);
    return true;
}

bool Dumping() { return CurrentShellSettings().debugMenuDump != 0; }

// A removed device's UINT64_MAX frees no slot (final review, Codex C1: menu_fence.h).
NativeSlot *PeekNative(Gpu &g) {
    NativeSlot &slot = g.nativeRing[g.nativeNext % kRing];
    return MenuFencePassed(g.f3.Get(), slot.value3) ? &slot : nullptr;
}
PrivateSlot *PeekPrivate(Gpu &g) {
    PrivateSlot &slot = g.privateRing[g.privateNext % kRing];
    return MenuFencePassed(g.f2.Get(), slot.value2) ? &slot : nullptr;
}

bool HostPresent(Pipeline &p, Gpu &g, ID3D12Resource *back, unsigned long long index, bool probe) {
    NativeSlot *slot = PeekNative(g);
    if (!slot || !Begin(slot->tailAllocator.Get(), slot->tailList.Get())) return false;
    bool dumped = MenuDumpRecord(slot->tailList.Get(), back, index, probe);
    dumped = SUCCEEDED(slot->tailList->Close()) && dumped;
    if (!dumped) { MenuDumpDiscard(); return false; }
    ID3D12CommandList *lists[] = {slot->tailList.Get()};
    g.presentQueue->ExecuteCommandLists(1, lists);
    if (SignalNative(g, *slot)) return true;
    Stop(p, index, "present-queue signal of f3");
    return false;
}

namespace {
// Present queue: the write-back of g.output (only after a successful wait on its pass, which the present queue has
// enqueued in order before this list), then a requested dump (of the written or the untouched frame), then f3.
void Tail(Pipeline &p, Gpu &g, NativeSlot &ns, ID3D12Resource *back, unsigned long long index, bool writeBack, bool dumpWanted) {
    const bool tailOpen = Begin(ns.tailAllocator.Get(), ns.tailList.Get());
    bool tail = false, dumped = false;
    if (tailOpen && writeBack) {
        Barrier(ns.tailList.Get(), back, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
        ns.tailList->CopyResource(back, g.output.Get());
        Barrier(ns.tailList.Get(), back, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
        tail = true;
    }
    if (tailOpen && dumpWanted && g.dumps && MenuDumpRecord(ns.tailList.Get(), back, index, false)) tail = dumped = true;
    if (tailOpen) {
        const bool closed = SUCCEEDED(ns.tailList->Close());
        if (tail && closed) {
            ID3D12CommandList *lists[] = {ns.tailList.Get()};
            g.presentQueue->ExecuteCommandLists(1, lists);
        } else { MenuDumpDiscard(); dumped = false; }
    }
    if (!SignalNative(g, ns)) { Stop(p, index, "present-queue signal of f3"); return; }
    if (dumped) p.menuDumpPending = false;
    const HRESULT removed = p.proxy->GetDeviceRemovedReason();
    if (removed != S_OK && EndSession(p, index, "the GPU device was removed"))
        Log("menu pipeline: DEVICE REMOVED 0x%08X at present %llu; menu mode off", unsigned(removed), index);
}

// Fix round 1 (review C2): the core may retire GPU objects (and its descriptor slots come back) during its evaluate, and
// its gates do not name the private queue, so no core call starts while an earlier menu pass may still run. The last
// pass's output is shown again instead (menus are near-static; an untouched frame would flicker NR off and on); before
// this run's first pass the frame stays untouched. Its write-back follows that pass through the present queue's Wait.
void ShowLastPass(Pipeline &p, Gpu &g, ID3D12Resource *back, unsigned long long index) {
    NativeSlot *ns = PeekNative(g);
    if (!ns) { ++p.skips; return; } // a pending dump stays pending
    ++p.reused;
    if (!p.loggedReuse) {
        p.loggedReuse = true;
        Log("menu pipeline: the previous menu pass was still on the GPU at present %llu; no core call, the last menu output is shown again (logged once)", index);
    }
    MenuEvent("reuse %llu %d", index, p.runPassed ? 1 : 0);
    const double t = NowMs();
    Tail(p, g, *ns, back, index, p.runPassed, p.menuDumpPending);
    p.phase[kTail] = NowMs() - t;
}
} // namespace

void MenuPresent(Pipeline &p, Gpu &g, ID3D12Resource *back, unsigned long long index, bool corePass) {
    // DebugMenuPass=3: the f2 signal failure of the first run's 10th present is decided here, before the capture list is
    // submitted and before the pass is recorded: this present submits nothing (no capture, no pass, no wait, no signal), so
    // the check never creates an unfenced submission. The frame stays untouched; only the requested dump of it is copied.
    if (MenuPassSetting() == MenuPassKind::ForcedSignalFailure && p.runs == 1 && p.runPresents == kDumpMenuPresent) {
        const bool dumpWanted = p.menuDumpPending; // dumped below, so not recorded as missed by the run's end
        p.menuDumpPending = false;
        Stop(p, index, "private signal of f2 (forced, DebugMenuPass=3)");
        if (dumpWanted && g.dumps && !HostPresent(p, g, back, index, false)) MenuEvent("missed %llu menu", index);
        return;
    }
    if (corePass && !MenuFencePassed(g.f2.Get(), g.v2)) { ShowLastPass(p, g, back, index); return; }
    NativeSlot *ns = PeekNative(g);
    PrivateSlot *ps = PeekPrivate(g);
    if (!ns || !ps) {
        ++p.skips;
        if (!p.loggedRing) { p.loggedRing = true; Log("menu pipeline: ring full at present %llu; present skipped (logged once)", index); }
        return; // a pending dump stays pending
    }
    // Present queue: the host's finished frame -> capture.
    double t = NowMs();
    if (!Begin(ns->captureAllocator.Get(), ns->captureList.Get())) return;
    Barrier(ns->captureList.Get(), back, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
    ns->captureList->CopyResource(g.capture.Get(), back);
    Barrier(ns->captureList.Get(), back, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
    if (FAILED(ns->captureList->Close())) return;
    ID3D12CommandList *capture[] = {ns->captureList.Get()};
    g.presentQueue->ExecuteCommandLists(1, capture);
    if (!Signal(g.presentQueue.Get(), g.f1.Get(), g.v1)) { // the capture only read the back buffer; nothing waits on f1
        SignalNative(g, *ns);
        Stop(p, index, "present-queue signal of f1"); // the run's end records a pending dump as missed
        return;
    }
    const bool dumpWanted = p.menuDumpPending; // a stop below still dumps the untouched frame
    p.phase[kCapture] = NowMs() - t; t = NowMs();
    // Private queue: the pass, submitted before the present queue is told to wait for it.
    bool wrote = true;
    const bool recorded = RecordPass(g, *ps, &wrote);
    p.phase[kEvaluate] = MenuModelTakeEvaluateMs();
    p.phase[kRecord] = NowMs() - t - p.phase[kEvaluate]; t = NowMs();
    const bool waited = recorded && SubmitPass(p, g, *ps, index);
    p.phase[kSubmit] = NowMs() - t; t = NowMs();
    if (!recorded) {
        MenuGuidesUnused();
        ++p.failures;
        if (!p.loggedFail) {
            p.loggedFail = true;
            Log("menu pipeline: the pass refused at present %llu%s; private list not executed, no wait enqueued, frame left untouched (logged once)",
                index, MenuPassSetting() == MenuPassKind::ForcedRefusal ? " (forced failure, DebugMenuPass=2)" : "");
            MenuEvent("fail %llu %s", index, MenuPassSetting() == MenuPassKind::ForcedRefusal ? "forced" : "pass");
        }
    }
    if (waited && wrote) p.runPassed = true; // g.output now holds (once f2 passes) a pass of this run
    // A pass that left g.output alone shows this run's previous output; before the run's first one, nothing is written.
    Tail(p, g, *ns, back, index, waited && p.runPassed, dumpWanted);
    p.phase[kTail] = NowMs() - t;
}

} // namespace ofps::reshade::menu_detail
