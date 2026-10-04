#include "hosts/reshade/menu_pipeline_state.h"
#include "hosts/reshade/menu_core_pass.h"
#include "hosts/reshade/menu_dump.h"
#include "hosts/reshade/menu_params.h"
#include "hosts/reshade/menu_settings.h"
#include "hosts/reshade/direct_host.h"
#include "hosts/reshade/ngx_hook_api.h"
#include "hosts/reshade/shell_host.h"
#include <memory>

// One present under the state machine (menu_state.h): the blockers, entry and the end of a run (C5), the host queue's
// entry fence (C2), the D3D12 resources for this frame, then the menu pass (menu_submit.cpp) or a dump outside a menu.
// D3D11 swap chains continue in menu_pipeline_d3d11.cpp. Under the pipeline's lock.
namespace ofps::reshade::menu_detail {
// A run's first pass waits for the host's last NR work on its own queue (menu_host_queue.h). False: no pass now.
bool OrderRunEntry(Pipeline &p, ID3D12CommandQueue *presentQueue, ID3D12CommandQueue *privateQueue, unsigned long long index) {
    switch (HostQueue().OrderEntry(p.proxy, presentQueue, privateQueue)) {
    case MenuEntryOrder::WaitingForHostQueue:
        // The run ends on this very present with the blocker in the tab; Blocker raises it from the next present on,
        // until the queue is seen executing the host's latest NR list.
        p.hostQueueWaiting = true;
        p.state.Block(kMenuHostQueueBlocker, true);
        EndRun(p, index, "left");
        return false;
    case MenuEntryOrder::Failed:
        Stop(p, index, "the entry fence on the host's NR queue");
        return false;
    case MenuEntryOrder::Ordered:
        if (!p.loggedHostQueue) {
            p.loggedHostQueue = true;
            Log("menu mode: the host runs NR on its own queue %p; menu passes are fenced against it", (void *) HostQueue().Queue());
        }
        break;
    case MenuEntryOrder::NotNeeded: break;
    }
    p.orderEntry = false;
    return true;
}

// Fix round 3: a host evaluate of the watched feature holds a lease between its proof and the core's evaluate; no pass
// now (the frame stays untouched, nothing waits).
bool LeaseSkips(Pipeline &p, unsigned long long index) {
    if (!p.leases.Held()) return false;
    ++p.skips;
    if (!p.loggedLease) {
        p.loggedLease = true;
        Log("menu pipeline: a host evaluate of the watched feature is under way at present %llu; menu pass skipped (logged once)", index);
    }
    return true;
}

namespace {
void StepD3D12(Pipeline &p, ::reshade::api::command_queue *queue, ::reshade::api::swapchain *swapchain, unsigned long long index) {
    auto *native = reinterpret_cast<ID3D12CommandQueue *>(queue->get_native());
    auto *back = reinterpret_cast<ID3D12Resource *>(swapchain->get_current_back_buffer().handle);
    if (!native || !back) return;
    const D3D12_RESOURCE_DESC desc = back->GetDesc();
    const MenuColourSpace space = SpaceOf(swapchain->get_color_space());
    const std::shared_ptr<const MenuParamSnapshot> snapshot = MenuBook().Newest();
    bool retryable = false;
    const char *blocker = Blocker(p, snapshot.get(), UINT(desc.Width), desc.Height, desc.Format, space, &retryable);
    const MenuStep step = p.state.Present(NowMs(), p.remembered && snapshot != nullptr, blocker, retryable);
    if (step.left) EndRun(p, index, "left");
    if (step.enter) OnEnter(p, index);
    if (!p.state.Enabled() || p.state.Stopped()) { // the checkbox off (or a stop): size-bound resources retire on their fence
        if (p.gpu && p.gpu->resources) RetireResources(*p.gpu);
        return;
    }
    if (!p.remembered || !snapshot || blocker) return;
    if (!p.gpu && !Build(p, native)) {
        Gpu *failed = p.gpu;
        if (!failed->poisoned) { // nothing was submitted (else: the warm-up signal failed and may still be in flight)
            if (failed->registered && Core()) Core()->UnregisterQueue(failed->queue.Get());
            delete failed;
            p.gpu = nullptr;
        }
        Stop(p, index, "building the private queue", "menu mode's GPU objects could not be created (see ReShade.log)");
        return;
    }
    if (!p.bound) p.bound = swapchain; // final review (Codex I3): menu mode now serves this swap chain until it is destroyed
    Gpu &g = *p.gpu;
    if (native != g.presentQueue.Get()) {
        if (!p.loggedQueue) { p.loggedQueue = true; Log("menu pipeline: a second present queue %p; ignored (logged once)", (void *) native); }
        return;
    }
    if (g.resources && (desc.Width != g.width || desc.Height != g.height || desc.Format != g.format || space != g.space ||
                        snapshot->shape.colour.Format != g.modelIn || snapshot->shape.output.Format != g.modelOut)) {
        Log("menu pipeline: back buffer %llux%u format %d (%s) -> %llux%u format %d (%s); rebuilding once the GPU retired the old resources",
            (unsigned long long) g.width, g.height, int(g.format), MenuColourSpaceName(g.space), desc.Width, desc.Height, int(desc.Format),
            MenuColourSpaceName(space));
        RetireResources(g);
        if (p.state.Active()) { p.state.Interrupt(); EndRun(p, index, "resize"); } // entered again once rebuilt
    }
    if (!g.resources) {
        if (!g.graveyard.empty() || !MenuDumpIdle()) return; // until f3 retired the old ones
        if (!BuildResources(p, g, desc, space, snapshot->shape)) {
            p.buildProblem = "menu mode's textures could not be created for this frame (see ReShade.log)";
            p.buildKey = BuildKey(UINT(desc.Width), desc.Height, desc.Format, space, snapshot.get());
            return;
        }
        p.buildProblem.clear();
    }
    const bool dumpOn = Dumping() && g.dumps;
    if (step.run && p.state.Active()) {
        if (LeaseSkips(p, index)) return;
        if (p.orderEntry && !OrderRunEntry(p, g.presentQueue.Get(), g.queue.Get(), index)) return;
        ++p.runPresents; ++p.menuPresents;
        if (dumpOn && p.runPresents == kDumpMenuPresent) p.menuDumpPending = true;
        MenuModelBind(snapshot);
        MenuPresent(p, g, back, index, MenuPassRunsModel() && MenuUsesCore(snapshot->shape, UINT(desc.Width), desc.Height, MenuCadenceMode()));
        return;
    }
    const bool afterExit = p.afterExit > 0;
    if (afterExit) --p.afterExit;
    // The after-exit frames are fixed (the first 5 after an exit): one not copied is recorded as missed.
    if (dumpOn && (afterExit || index == kProbePresent) && !HostPresent(p, g, back, index, !afterExit)) MenuEvent("missed %llu", index);
}
} // namespace

void Step(Pipeline &p, ::reshade::api::command_queue *queue, ::reshade::api::swapchain *swapchain, unsigned long long index) {
    if (!BoundTo(p, swapchain)) return; // before the state machine: another swap chain neither enters nor ends a run
    const auto api = swapchain->get_device()->get_api();
    if (api == ::reshade::api::device_api::d3d11) { StepD3D11(p, queue, swapchain, index, SpaceOf(swapchain->get_color_space())); return; }
    if (api == ::reshade::api::device_api::d3d12) { StepD3D12(p, queue, swapchain, index); return; }
    if (p.state.Present(NowMs(), p.remembered, "not a D3D12 or D3D11 swap chain", false).left) EndRun(p, index, "left");
}

bool BoundTo(Pipeline &p, const void *swapchain) {
    if (!p.bound || swapchain == p.bound) return true;
    if (!p.loggedUnbound) {
        p.loggedUnbound = true;
        Log("menu pipeline: a second swap chain %p presents; ignored while menu mode serves %p, until that one is destroyed (logged once)",
            swapchain, p.bound);
    }
    return false;
}

void FollowCheckbox(Pipeline &p, bool on) {
    p.state.SetEnabled(on);
    if (p.wasEnabled && !on) MenuBook().Withdraw(MenuBook().Tag()); // the book's leaf lock, under the pipeline's
    p.wasEnabled = on;
}

MenuColourSpace SpaceOf(::reshade::api::color_space space) {
    using ::reshade::api::color_space;
    switch (space) {
    case color_space::srgb: return MenuColourSpace::Srgb;
    case color_space::scrgb: return MenuColourSpace::Scrgb;
    case color_space::hdr10_pq: return MenuColourSpace::Hdr10Pq;
    case color_space::hdr10_hlg: return MenuColourSpace::Hdr10Hlg;
    default: return MenuColourSpace::Unknown;
    }
}

bool EndRun(Pipeline &p, unsigned long long index, const char *why) {
    if (p.menuDumpPending) { p.menuDumpPending = false; MenuEvent("missed %llu menu", index); }
    p.orderEntry = false;
    // C5 (Task 13): the only place a run switches the core's motion source back to the game's vectors, before the game's
    // next evaluate (an exit calls this from that evaluate's hook, before the core runs it). No-op when the run never
    // switched it on (a run without the sync cadence, or one that never reached the core). A refused switch-back is owed
    // (fix round 1): the host evaluate retries it before the core runs the game's frame.
    const bool restored = MenuCoreFlow(false);
    MenuCorePassRunEnd();
    MenuEvent("%s %llu %.1f", why, index, NowMs() - p.enterMs);
    if (Dumping()) {
        MenuEvent("flow %s %llu", restored ? "off" : "owed", index);
        p.noteGameEvaluate = true; // the teardown check: the next game evaluate reached the core with the game's vectors
        Log("menu mode: run %u ended (%s) at present %llu after %u menu presents", p.runs, why, index, p.runPresents);
    }
    return restored;
}

void OnEnter(Pipeline &p, unsigned long long index) {
    const double now = NowMs();
    p.runPresents = 0; p.orderEntry = true; p.enterMs = now; ++p.runs; p.runPassed = false;
    MenuGpuTimeRunStart();
    MenuCorePassRunStart(); // stage 3: the run's first core pass resets the core's history and NR's
    MenuEvent("enter %llu %.1f", index, p.state.MsSinceEvaluate(now));
    if (p.loggedFirstRun && !Dumping()) return; // one line for the session's first run; every run with DebugMenuDump=1
    p.loggedFirstRun = true;
    Log("menu mode: run %u entered at present %llu (%u presents, %.1f ms without a host evaluate)", p.runs, index,
        p.state.QuietPresents(), p.state.MsSinceEvaluate(now));
    p.phase[kLog] = NowMs() - now;
}

bool EndSession(Pipeline &p, unsigned long long index, const char *reason) {
    if (p.state.Stopped()) return false;
    const bool active = p.state.Active();
    p.state.Stop(reason);
    if (active) EndRun(p, index, "stop");
    else {
        p.menuDumpPending = false;
        MenuEvent("stop %llu", index);
    }
    return true;
}

void Stop(Pipeline &p, unsigned long long index, const char *what, const char *reason) {
    if (EndSession(p, index, reason))
        Log("menu pipeline: %s failed at present %llu; frame left untouched, menu mode off for the session", what, index);
}

void Quarantine(Pipeline &p, unsigned long long index, const char *what) {
    if (p.gpu) p.gpu->poisoned = true; // the GPU may still run the last pass: nothing of the pipeline is released again
    if (p.bridge) QuarantineD3D11(p);
    EndSession(p, index, "the last menu pass could not be proven finished (see ReShade.log)");
    if (p.quarantined) return; // logged once: a removed device answers every later evaluate the same way
    p.quarantined = true;
    Log("menu pipeline: %s; the pipeline's objects are kept for the session, menu mode off for the session", what);
}

unsigned long long BuildKey(UINT width, UINT height, DXGI_FORMAT format, MenuColourSpace space, const MenuParamSnapshot *snapshot) {
    return (static_cast<unsigned long long>(width) << 44) ^ (static_cast<unsigned long long>(height) << 28) ^
           (static_cast<unsigned long long>(format) << 16) ^ (static_cast<unsigned long long>(space) << 12) ^
           (snapshot ? snapshot->tag.generation : 0ull);
}

const char *Blocker(Pipeline &p, const MenuParamSnapshot *snapshot, UINT width, UINT height, DXGI_FORMAT format, MenuColourSpace space,
                    bool *retryable) {
    *retryable = false;
    if (DirectHostActive()) return "OptiScaler runs the model itself here (direct host)";
    if (SafeMode()) return "the crash guard's safe mode is on";
    // Colour and shape bind the model pass only (the marker and forced-refusal checks run on any swap chain); both are
    // re-checked every present: HDR10 now, SDR later; a compressed model now, full size later.
    const bool model = MenuPassRunsModel();
    if (const char *colour = model ? MenuColourProblem(space, format) : nullptr) { *retryable = true; return colour; }
    if (p.snapshotProblem) return p.snapshotProblem;
    if (snapshot && model) {
        if (const char *shape = MenuShapeProblem(snapshot->shape, width, height)) { *retryable = true; return shape; }
        if (MenuConvertViewFormat(snapshot->shape.colour.Format) == DXGI_FORMAT_UNKNOWN ||
            MenuConvertViewFormat(snapshot->shape.output.Format) == DXGI_FORMAT_UNKNOWN)
            return "the game's NR colour format is not supported";
        // Task 13: menus never run the background temporal mode; the core would have to for a compressed model.
        const int temporal = MenuTemporalMode();
        if (const char *background = MenuTemporalProblem(snapshot->shape, width, height, temporal)) { *retryable = true; return background; }
        if (MenuUsesCore(snapshot->shape, width, height, MenuCadenceMode())) { // stage 3: the core's evaluate runs the menu frame
            // C6: the sync cadence without optical flow would carry frames along the block's zero motion. Mode Off falls
            // back to the direct pass on every frame (MenuCadenceMode); a compressed model has no such fallback.
            if (temporal == 1) if (const char *flow = MenuFlowProblem()) return flow;
            // Fix round 1: a setting the game's last evaluate did not apply would make the core re-lay out the feature
            // (re-create its model, retire GPU objects a menu pass may still read) inside the menu call. The game's
            // next evaluate applies it; its snapshot lifts this.
            if (snapshot->settingsEpoch != MenuSettingsEpoch()) {
                *retryable = true;
                return "a setting changed in this menu; it applies from the game's next frame";
            }
        }
    }
    if (!p.buildProblem.empty() && p.buildKey == BuildKey(width, height, format, space, snapshot)) return p.buildProblem.c_str();
    if (p.hostQueueWaiting) { // C2: raised by a run's entry, lifted once ReShade saw the host's latest NR list executing
        if (HostQueue().EntryWaiting()) { *retryable = true; return kMenuHostQueueBlocker; }
        p.hostQueueWaiting = false;
    }
    return nullptr;
}

} // namespace ofps::reshade::menu_detail
