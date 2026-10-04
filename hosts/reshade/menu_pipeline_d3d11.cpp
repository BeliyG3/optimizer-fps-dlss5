#include "hosts/reshade/menu_pipeline_state.h"
#include "hosts/reshade/menu_bridge_d3d11.h"
#include "hosts/reshade/menu_core_pass.h"
#include "hosts/reshade/menu_dump.h"
#include "hosts/reshade/menu_marker.h"
#include "hosts/reshade/menu_params.h"
#include "hosts/reshade/shell_host.h"
#include <reshade_api.hpp>

// The menu pipeline on a D3D11 swap chain (stage 2): the same state machine, blockers and ends of a run as on D3D12
// (menu_pipeline_step.cpp), the frame carried to the host list's D3D12 device and back through MenuBridgeD3D11, with the
// D3D12 path's safety (Task 10):
//   - a missing piece of the bridge is a per-swap-chain blocker retried when the swap chain or the feature's generation
//     changes (BuildKey), not a session stop; the colour check is the retryable one in Blocker;
//   - C2: a run's first pass waits for the host's NR queue (OrderRunEntry: "waiting for the host's NR queue" until
//     ReShade saw it executing); the exit and the model's re-creation or release follow the last pass through the proof
//     record p.outstanding (menu_pipeline_host.cpp), noted here after every fenced pass;
//   - C1: the private queue is registered with the core (menu_pipeline_d3d11_gpu.cpp), each pass's list reported to it;
//   - stage 3: a compressed model's menu frame goes through the core, never while the previous pass may still run (the
//     last output is shown again instead).
// All under the pipeline's lock.
namespace ofps::reshade::menu_detail {
namespace {
bool ForcedFailure(MenuFrame &) { return false; } // DebugMenuPass=2

// Size-bound resources: the bridge's pair, the model pass, the marker (DebugMenuPass=1) or a warm-up of the
// private queue, the dump pool. Null on success, else why the bridge is unavailable.
const char *Open(Pipeline &p, MenuBridgeD3D11 &b, ID3D11Device *d11, const D3D11_TEXTURE2D_DESC &desc, MenuColourSpace space,
                 const MenuShape &shape) {
    if (desc.SampleDesc.Count != 1) return "a multisampled back buffer";
    if (MenuPassSetting() == MenuPassKind::ForcedSignalFailure) return "the forced signal failure (DebugMenuPass=3) is not wired on the bridge";
    if (!b.Open(d11, p.proxy, desc.Width, desc.Height, desc.Format)) return b.Reason();
    if (!b.Registered()) RegisterBridgeD3D11(p, b); // C1: before the queue's first submission (the warm-up below)
    if (!b.Timed()) { // once per private queue (a partial device failure re-creates the queue: review M2)
        b.MarkTimed();
        if (!MenuGpuTimeBuild(p.proxy, b.Queue(), 3)) Log("menu pipeline: no GPU timestamps on the private queue");
    }
    if (MenuPassRunsModel()) {
        D3D12_RESOURCE_DESC back{}; back.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; back.Width = desc.Width; back.Height = desc.Height;
        back.DepthOrArraySize = 1; back.MipLevels = 1; back.Format = desc.Format; back.SampleDesc.Count = 1;
        if (!MenuModelBuild(p.proxy, back, shape)) return "model pass resources unavailable";
    }
    // The marker pass uploads its texture; every pass warms the private queue (its first submission costs 1.5-2.7 ms of CPU).
    const bool setup = b.Setup([&](ID3D12GraphicsCommandList *list, MenuBridgeD3D11::Objects &retire, MenuBridgeD3D11::Objects &keep) {
        if (MenuPassSetting() != MenuPassKind::Marker) return true;
        ComPtr<ID3D12Resource> marker, upload;
        const bool made = MenuMarkerCreate(p.proxy, desc.Format, list, &marker, &upload);
        if (upload) retire.emplace_back(upload.Get());
        if (!made) return false;
        keep.emplace_back(marker.Get());
        MenuMarkerBind(marker.Get());
        return true;
    });
    if (!setup) {
        if (b.Poisoned()) return b.Reason(); // a failed Wait/Signal: StepD3D11 stops menu mode for the session
        return MenuPassSetting() == MenuPassKind::Marker ? "marker texture unavailable" : "private queue warm-up failed";
    }
    p.dumps11 = Dumping() && MenuDump11Prepare(d11, desc.Width, desc.Height, desc.Format);
    p.space11 = space;
    p.modelIn11 = shape.colour.Format; p.modelOut11 = shape.output.Format;
    MenuBridgeDebugMessages(d11, p.proxy, "open");
    return nullptr;
}

void Retire(Pipeline &p, MenuBridgeD3D11 &b) {
    b.Retire(MenuModelRetire());
    MenuMarkerBind(nullptr);
    p.dumps11 = false;
}

// After a Present that executed the private pass, as SubmitPass does on D3D12 (menu_submit.cpp): the core counts the list
// on the registered queue (C1), the exit reset is owed, and the pass becomes the proof record; a pass whose Signal(b)
// failed gets a recovery fence (C3; the watched feature's evaluates are withheld until it passed).
void AfterPass(Pipeline &p, MenuBridgeD3D11 &b, unsigned long long index) {
    if (!b.Submitted()) return;
    // Always reported on our own registration (fix round 2), which exists before the queue's first ExecuteCommandLists.
    // If ReShade also reports the list (a queue of its wrapped device, native identity), the core's Submission::Push
    // claims each tagged recording once (r.serial == 0) and a second push only raises that queue's serial, signalled
    // behind the list on the same GPU queue: a duplicate is harmless, a missed report is not.
    if (b.Registered())
        if (IOfpsCore *core = Core()) core->OnCommandListExecuted(b.Queue(), b.LastList());
    p.state.NotePassSubmitted(); // the model ran on the menu frame: the next host evaluate resets NR history
    if (b.Fenced()) { p.outstanding.NotePass(b.Fence(), b.PassValue()); return; }
    const bool fenced = p.outstanding.Recover(p.proxy, b.Queue());
    p.loggedWithhold = false;
    Log("menu pipeline: the bridge's Signal(b) failed after its pass was submitted at present %llu; %s", index,
        fenced ? "a recovery fence follows the pass; the game's NR evaluates wait for it (withheld until it passed)"
               : "the recovery signal failed too; the game's NR evaluates of this feature are withheld for the session");
}

// Stage 3 (Task 12 fix round 1 on D3D11): the previous pass may still run, so no core call; the last output is shown again
// (before this run's first pass the frame stays untouched). Skipped: nothing was enqueued.
MenuBridgeResult ShowLastPassD3D11(Pipeline &p, MenuBridgeD3D11 &b, ID3D11DeviceContext *ctx, ID3D11Resource *back, unsigned long long index) {
    ++p.reused;
    if (!p.loggedReuse) {
        p.loggedReuse = true;
        Log("menu pipeline: the previous menu pass was still on the GPU at present %llu; no core call, the last menu output is shown again (logged once)", index);
    }
    MenuEvent("reuse %llu %d", index, p.runPassed ? 1 : 0);
    return b.Reshow(ctx, back, p.runPassed);
}

void MenuPresentD3D11(Pipeline &p, MenuBridgeD3D11 &b, ID3D11DeviceContext *ctx, ID3D11Resource *back, unsigned long long index, bool corePass) {
    MenuBridgeResult result;
    if (corePass && !b.PassDone()) { // a removed device's fence never counts as done (menu_fence.h)
        result = ShowLastPassD3D11(p, b, ctx, back, index);
        if (result == MenuBridgeResult::Skipped) result = MenuBridgeResult::Written; // the untouched frame; its dump is taken
    } else {
        const MenuPassKind kind = MenuPassSetting();
        const MenuPass pass = kind == MenuPassKind::Marker ? MenuMarkerPass : kind == MenuPassKind::ForcedRefusal ? ForcedFailure : MenuModelPass;
        result = b.Present(ctx, back, pass, p.runPassed);
        AfterPass(p, b, index); // Written, or Failed after the private pass ran
        // `out` holds (once b passed) a pass of this run, unless the pass left it alone (MenuFrame::wrote).
        if (result == MenuBridgeResult::Written && b.PassWrote()) p.runPassed = true;
    }
    p.phase[kEvaluate] = MenuModelTakeEvaluateMs();
    p.phase[kRecord] = b.recordMs - p.phase[kEvaluate];
    p.phase[kSubmit] = b.submitMs;
    if (b.submitMs >= 0.5) MenuEvent("bridgeslow %llu d11in %.4f d12 %.4f d11out %.4f", index, b.submitParts[0], b.submitParts[1], b.submitParts[2]);
    switch (result) {
    case MenuBridgeResult::Skipped:
        ++p.skips;
        if (!p.loggedRing) { p.loggedRing = true; Log("menu pipeline: ring full at present %llu; present skipped (logged once)", index); }
        return; // a pending dump stays pending
    case MenuBridgeResult::Refused:
        ++p.failures;
        if (!p.loggedFail) {
            p.loggedFail = true;
            Log("menu pipeline: the pass refused at present %llu%s; private list not executed, no wait enqueued on either side, frame left untouched (logged once)",
                index, MenuPassSetting() == MenuPassKind::ForcedRefusal ? " (forced failure, DebugMenuPass=2)" : "");
            MenuEvent("fail %llu %s", index, MenuPassSetting() == MenuPassKind::ForcedRefusal ? "forced" : "pass");
        }
        break;
    case MenuBridgeResult::Failed: {
        const bool dumpWanted = p.menuDumpPending; // the frame is still dumped below, so the run's end does not count it missed
        p.menuDumpPending = false;
        if (!b.WroteBack()) Stop(p, index, b.Reason()); // the frame stays untouched
        else if (EndSession(p, index, "stopped after a GPU fence error (see ReShade.log)")) // Signal(c) failed after the write-back
            Log("menu pipeline: %s failed at present %llu after the write-back was enqueued (the frame shows the pass); menu mode off for the session",
                b.Reason(), index);
        p.menuDumpPending = dumpWanted;
        break;
    }
    case MenuBridgeResult::Written: break;
    }
    const double t = NowMs();
    if (p.menuDumpPending && p.dumps11 && MenuDump11Record(ctx, back, index, false)) p.menuDumpPending = false;
    p.phase[kTail] = NowMs() - t;
    const HRESULT removed12 = p.proxy->GetDeviceRemovedReason(), removed11 = b.Device11()->GetDeviceRemovedReason();
    if ((removed12 != S_OK || removed11 != S_OK) && EndSession(p, index, "the GPU device was removed"))
        Log("menu pipeline: DEVICE REMOVED (D3D12 0x%08X, D3D11 0x%08X) at present %llu; menu mode off", unsigned(removed12), unsigned(removed11), index);
}
} // namespace

void StepD3D11(Pipeline &p, ::reshade::api::command_queue *queue, ::reshade::api::swapchain *swapchain, unsigned long long index,
               MenuColourSpace space) {
    auto *ctx = reinterpret_cast<ID3D11DeviceContext *>(queue->get_native());
    auto *d11 = reinterpret_cast<ID3D11Device *>(swapchain->get_device()->get_native());
    auto *back = reinterpret_cast<ID3D11Resource *>(swapchain->get_current_back_buffer().handle);
    ComPtr<ID3D11Texture2D> texture;
    if (!ctx || !d11 || !back || FAILED(back->QueryInterface(IID_PPV_ARGS(&texture)))) return;
    // Review M4: a swap chain of another D3D11 device than the bridge's is ignored before the state machine (as D3D12's
    // second present queue), so it neither ends the run nor flips the tab's status every present.
    if (p.bridge && p.bridge->Device11Raw() && d11 != p.bridge->Device11Raw()) {
        if (!p.loggedOther11) {
            p.loggedOther11 = true;
            Log("menu pipeline: a swap chain of a second D3D11 device %p; ignored while the bridge serves %p (logged once)", (void *) d11,
                (void *) p.bridge->Device11Raw());
        }
        return;
    }
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    const std::shared_ptr<const MenuParamSnapshot> snapshot = MenuBook().Newest();
    bool retryable = false;
    const char *blocker = Blocker(p, snapshot.get(), desc.Width, desc.Height, desc.Format, space, &retryable);
    const MenuStep step = p.state.Present(NowMs(), p.remembered && snapshot != nullptr, blocker, retryable);
    if (step.left) EndRun(p, index, "left");
    if (step.enter) OnEnter(p, index);
    if (!p.state.Enabled() || p.state.Stopped()) { // the checkbox off (or a stop): the pair retires on its fence
        if (p.bridge && p.bridge->Opened()) { Retire(p, *p.bridge); p.bridge->FlushSignals(); }
        return;
    }
    if (!p.remembered || !snapshot || blocker) return;
    if (!p.bridge) {
        p.bridge = new MenuBridgeD3D11();
        p.bound = swapchain; // final review (Codex I3): menu mode now serves this swap chain until it is destroyed
        Log("menu pipeline: D3D11 swap chain (device %p, immediate context %p); the frame goes through a D3D11 <-> D3D12 bridge to the host list's device %p",
            (void *) d11, (void *) ctx, (void *) p.proxy);
    }
    MenuBridgeD3D11 &b = *p.bridge;
    p.context11 = ctx;
    if (b.Opened() && (desc.Width != b.Width() || desc.Height != b.Height() || desc.Format != b.Format() || space != p.space11 ||
                       snapshot->shape.colour.Format != p.modelIn11 || snapshot->shape.output.Format != p.modelOut11)) {
        Log("menu pipeline: back buffer %ux%u format %d -> %ux%u format %d (%s); the bridge reopens once the GPU retired the old pair",
            b.Width(), b.Height(), int(b.Format()), desc.Width, desc.Height, int(desc.Format), MenuColourSpaceName(space));
        Retire(p, b);
        b.FlushSignals(); // the old pair retires at a value the D3D11 buffer may still hold
        if (p.state.Active()) { p.state.Interrupt(); EndRun(p, index, "resize"); } // entered again once reopened
    }
    if (!b.Opened()) {
        if (!b.Idle() || !MenuDump11Idle()) return; // until the fence retired the old pair
        if (const char *why = Open(p, b, d11, desc, space, snapshot->shape)) {
            Log("menu mode: D3D11 bridge unavailable (%s)", why);
            MenuEvent("unavailable %llu %s", index, why);
            if (b.Poisoned()) { // a fence operation failed (the warm-up): stop for the session, keep every object
                Stop(p, index, why, "the D3D11 bridge's private queue failed a fence operation (see ReShade.log)");
                return;
            }
            if (b.Opened()) Retire(p, b); // opened, then a later piece failed before any submission with the pair
            // Retried when the swap chain or the feature's generation changes (BuildKey), not every present.
            p.buildProblem = std::string("D3D11 bridge: ") + why;
            p.buildKey = BuildKey(desc.Width, desc.Height, desc.Format, space, snapshot.get());
            return;
        }
        p.buildProblem.clear();
    }
    if (step.enter) MenuBridgeDebugMessages(b.Device11(), p.proxy, "enter");
    const bool dumpOn = Dumping() && p.dumps11;
    if (step.run && p.state.Active()) {
        if (LeaseSkips(p, index)) return;
        // C2: renodx runs NR on the host bridge's own D3D12 queue; the run's first pass waits for its last NR work (the
        // Wait lands on the private queue before the bridge's own Wait(a) and the pass).
        if (p.orderEntry && !OrderRunEntry(p, nullptr, b.Queue(), index)) return;
        ++p.runPresents; ++p.menuPresents;
        if (dumpOn && p.runPresents == kDumpMenuPresent) p.menuDumpPending = true;
        MenuModelBind(snapshot);
        MenuPresentD3D11(p, b, ctx, back, index, MenuPassRunsModel() && MenuUsesCore(snapshot->shape, desc.Width, desc.Height, MenuCadenceMode()));
        return;
    }
    const bool afterExit = p.afterExit > 0;
    if (afterExit) --p.afterExit;
    if (dumpOn && (afterExit || index == kProbePresent) && !MenuDump11Record(ctx, back, index, !afterExit)) MenuEvent("missed %llu", index);
}

double PollD3D11(Pipeline &p, unsigned long long index) {
    MenuBridgeD3D11 &b = *p.bridge;
    const double io = p.context11 ? MenuDump11Poll(p.context11) : 0.0;
    if (b.Fence()) MenuGpuTimePoll(b.Completed(), index);
    b.Poll();
    if (!p.state.Active() && p.afterExit == kDumpAfterExit) MenuBridgeDebugMessages(b.Device11(), p.proxy, "exit"); // the first present after an exit
    return io;
}

void ForgetD3D11(Pipeline &p) {
    if (p.bridge->Fence())
        Log("menu pipeline: the watched feature is released with the bridge's last pass at b %llu (completed %llu)",
            (unsigned long long) p.bridge->PassValue(), (unsigned long long) p.bridge->Completed());
    if (p.bridge->Opened()) Retire(p, *p.bridge);
    if (p.bridge->Device11()) MenuBridgeDebugMessages(p.bridge->Device11(), p.proxy, "release");
}

MenuBridgeD3D11 *DetachD3D11(Pipeline &p) {
    MenuBridgeD3D11 *b = p.bridge;
    if (!b) return nullptr;
    p.bridge = nullptr; p.context11 = nullptr; // a run under way ended in MenuPipelineDrain (EndRun "drain")
    MenuMarkerBind(nullptr);
    b->Retire(MenuModelRetire()); // released by Drain once the fence passed
    b->FlushSignals();            // the last Signal(c) may still sit in the context's buffer: submitted before any wait
    return b;
}

} // namespace ofps::reshade::menu_detail
