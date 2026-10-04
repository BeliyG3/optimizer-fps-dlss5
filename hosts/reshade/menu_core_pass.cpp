#include "hosts/reshade/menu_core_pass.h"
#include "hosts/reshade/menu_dump.h"
#include "hosts/reshade/menu_settings.h"
#include "hosts/reshade/model_host_ngx.h"
#include "hosts/reshade/ngx_hook_api.h"
#include "hosts/reshade/ngx_params.h"
#include "hosts/reshade/shell_host.h"
#include "hosts/reshade/addon/shell_settings.h"
#include <cstdarg>
#include <cstdio>

// Everything here runs under the menu pipeline's lock (the present thread, and the host evaluate's AtHostEvaluate).
namespace ofps::reshade {
namespace {
struct CorePass {
    bool resetNext = true;      // a run's first model frame resets the core's temporal state and NR's history
    bool createBlocked = false; // a menu call met a model creation: no core pass until the game's next evaluate
    bool loggedBusy = false, loggedInputs = false, loggedRefused = false, loggedModel = false, loggedCreate = false;
    bool loggedFirst = false, loggedRedo = false, loggedSlots = false, loggedNoScope = false;
    unsigned frames = 0, modelFrames = 0, flowFrames = 0; // this run's frames through the core
};
CorePass &C() { static CorePass *pass = new CorePass(); return *pass; } // never destroyed from DllMain

void Log(const char *fmt, ...) {
    char text[384]; va_list args; va_start(args, fmt); std::vsnprintf(text, sizeof(text), fmt, args); va_end(args);
    Host().Log(OFPS_LOG_INFO, text);
}

bool Dumping() { return CurrentShellSettings().debugMenuDump != 0; }

// One core evaluate of the menu frame on the watched feature, with the menu block's model inputs.
struct CoreCall {
    bool called = false, refusedCreate = false, modelCalled = false, noScope = false;
    int rc = OFPS_E_STATE, model = kNgxSuccess;
    int slotMisses = 0; // descriptor-pool acquires the core refused instead of waiting (final review, Codex I1)
    OfpsEvalResult result{};
    bool ModelFrame() const { // the model ran and succeeded on this frame (a full frame, never a carried one)
        return called && !refusedCreate && rc >= 0 && modelCalled && model == kNgxSuccess && result.path != OFPS_PATH_CARRIED &&
               slotMisses == 0;
    }
};
// Final review (Codex I1): the core's descriptor pools wait up to 100 ms for a slot the GPU still uses; on the present
// path they must not. The core's private export makes this thread's acquires refuse instead (the core then records its
// fallback, which is never shown: MenuCorePass). Without the export (a core of another build) no core call is made.
CoreCall Evaluate(void *hostHandle, OwnParams &block, ID3D12GraphicsCommandList *list, const OfpsFrameInputs &in) {
    CoreCall call;
    call.result.size = sizeof(call.result);
    const auto nonBlocking = CoreNonBlockingDescriptors();
    IOfpsCore *const core = Core();
    if (!nonBlocking || !core) { call.noScope = true; return call; }
    call.called = WithFeatureForMenu(hostHandle, [&](IOfpsFeature &feature, ModelHostNgx &host) {
        if (nonBlocking(core, 1) != OFPS_OK) { call.noScope = true; return false; }
        host.BeginFrame(&block, nullptr); // the model inputs go into menu mode's block, never into the host's
        host.RefuseCreate(true);
        call.rc = feature.Evaluate(list, &in, &call.result);
        const int misses = nonBlocking(core, 0);
        call.slotMisses = misses < 0 ? 1 : misses; // an error answer counts as a miss (fail closed)
        call.refusedCreate = host.CreateRefused();
        host.RefuseCreate(false);
        call.modelCalled = host.ModelWasCalled();
        call.model = call.modelCalled ? host.LastNgxResult() : kNgxSuccess;
        // The core committed the new layout (or model-pass count) before the refused creation, so its next evaluate
        // would not retry: the game's next evaluate re-creates the model and its extra passes from the game's block.
        if (call.refusedCreate) feature.RequestModelRebuild();
        return true;
    });
    return call;
}

void LogCreateBlocked(CorePass &c) {
    c.createBlocked = true;
    if (c.loggedCreate) return;
    c.loggedCreate = true;
    Log("menu core pass: the core needed a new model inside a menu; it is created at the game's next NR frame (rebuild requested), menus stay untouched until then (logged once)");
}

// Fix round 1 (Codex I2): the core carried this frame without the optical flow field, i.e. along the menu's zero motion
// (its flow failed on this very frame; MenuFlowCheckFrame latched it). Its recording must run (the core counts it), but
// it never reaches the screen: the same list evaluates the frame again with the history reset, which makes the core plan
// a full frame (the model runs, NR resets), and that output overwrites the carried one. Should that fail, the pass keeps
// its output texture as it was, so the last menu output (this run's previous pass: a carried frame follows one) is shown
// again. From the next present the cadence is off (C6). DebugMenuNoFlow=3 skips the full frame to drive that fallback.
MenuCoreFrame CarriedWithoutFlow(CorePass &c, void *hostHandle, OwnParams &block, ID3D12GraphicsCommandList *list, OfpsFrameInputs in) {
    CoreCall redo;
    if (CurrentShellSettings().debugMenuNoFlow != 3) {
        in.hostReset = 1u;
        redo = Evaluate(hostHandle, block, list, in);
    }
    const bool full = redo.ModelFrame();
    if (Dumping()) MenuEvent("redo %d", full ? 1 : 0);
    if (redo.refusedCreate) { // as for any refused creation: nothing of this list runs, the frame stays untouched
        LogCreateBlocked(c);
        return MenuCoreFrame::Untouched;
    }
    if (full) {
        ++c.modelFrames;
        c.resetNext = false;
    } else {
        c.resetNext = true;
    }
    if (!c.loggedRedo || Dumping()) {
        c.loggedRedo = true;
        if (full) Log("menu core pass: a menu frame the core carried without the optical flow field ran again as a full model frame (path %u)",
                      unsigned(redo.result.path));
        else Log("menu core pass: a menu frame the core carried without the optical flow field is not shown (full model frame: called %d, evaluate %d, model 0x%08X, path %u); the last menu output is shown again",
                 redo.called ? 1 : 0, redo.rc, unsigned(redo.model), unsigned(redo.result.path));
    }
    return full ? MenuCoreFrame::Written : MenuCoreFrame::LastShownAgain;
}
} // namespace

void MenuCorePassRunStart() { C().resetNext = true; }

void MenuCorePassHostEvaluated() { C().createBlocked = false; }

bool MenuCorePassCreateBlocked() { return C().createBlocked; }

void MenuCorePassRunEnd() {
    CorePass &c = C();
    if (c.frames && Dumping()) {
        MenuEvent("cadence %u %u %u", c.frames, c.modelFrames, c.flowFrames);
        Log("menu core pass: the run had %u frames through the core, %u with the model, %u carried with the optical flow field",
            c.frames, c.modelFrames, c.flowFrames);
    }
    c.frames = c.modelFrames = c.flowFrames = 0;
}

MenuCoreFrame MenuCorePass(void *hostHandle, OwnParams &block, ID3D12GraphicsCommandList *list) {
    CorePass &c = C();
    if (c.createBlocked) return MenuCoreFrame::Untouched;
    // Task 13 (C6): the sync cadence carries frames only along the optical flow, never along the block's zero motion. The
    // path choice (MenuCadenceMode) and the Blocker keep such frames away when the flow is unavailable; this is the guard.
    const bool cadence = MenuTemporalMode() == 1;
    if (cadence && (MenuFlowProblem() || !MenuCoreFlow(true))) return MenuCoreFrame::Untouched;
    // The frame as the host path reads a block (subresources, the depth plane, both motion-scale spellings); the model
    // pass's textures and the snapshot's guides are in `block`. Reset: this run's own history, not the block's 0.
    OfpsFrameInputs in{};
    if (!ReadFrameInputs(&block, &in, nullptr)) {
        if (!c.loggedInputs) { c.loggedInputs = true; Log("menu core pass: the pass block lacks colour, depth, motion or output; the frame stays untouched (logged once)"); }
        return MenuCoreFrame::Untouched;
    }
    in.hostReset = c.resetNext ? 1u : 0u;
    const CoreCall call = Evaluate(hostHandle, block, list, in);
    if (call.noScope) {
        if (!c.loggedNoScope) {
            c.loggedNoScope = true;
            Log("menu core pass: the core DLL cannot run a menu frame without waiting for its descriptors (no OfpsNonBlockingDescriptorsV1); Uniform/Peripheral menus stay untouched (logged once)");
        }
        return MenuCoreFrame::Untouched;
    }
    if (!call.called) {
        if (!c.loggedBusy) { c.loggedBusy = true; Log("menu core pass: the feature is busy or gone; the frame stays untouched (logged once)"); }
        return MenuCoreFrame::Untouched;
    }
    if (call.refusedCreate) { LogCreateBlocked(c); return MenuCoreFrame::Untouched; } // the old model may already be retired
    // OFPS_E_ARG / OFPS_E_STATE: refused before anything the output depends on (an unexecuted list keeps any recording
    // the core tagged pending until the list runs again). Any other result recorded work that must run: the core's own
    // fallback (the frame copied through) on a failure.
    if (call.rc == OFPS_E_ARG || call.rc == OFPS_E_STATE) {
        if (!c.loggedRefused) { c.loggedRefused = true; Log("menu core pass: the core refused the menu frame (%d); the frame stays untouched (logged once)", call.rc); }
        return MenuCoreFrame::Untouched;
    }
    // Final review (Codex I1): a descriptor slot was still in use by the GPU, so the core recorded its fallback instead of
    // waiting. That work runs; its output is not shown (the last menu output again, or before the run's first pass the
    // untouched frame). The history reset stays owed.
    if (call.slotMisses > 0) {
        if (Dumping()) MenuEvent("slots %d", call.slotMisses);
        if (!c.loggedSlots) {
            c.loggedSlots = true;
            Log("menu core pass: no free descriptor slot (the GPU is behind); the core was not waited for, the last menu output is shown again (logged once)");
        }
        return MenuCoreFrame::LastShownAgain;
    }
    if ((call.rc < 0 || call.model != kNgxSuccess) && !c.loggedModel) {
        c.loggedModel = true;
        Log("menu core pass: evaluate %d, model 0x%08X; the core's own fallback is shown (logged once)", call.rc, unsigned(call.model));
    }
    if (!c.loggedFirst) {
        c.loggedFirst = true;
        Log("menu core pass: the first menu frame went through the core (evaluate %d, path %u, warp path %u, model 0x%08X)", call.rc,
            unsigned(call.result.path), unsigned(call.result.warpPath), unsigned(call.model));
    }
    ++c.frames;
    if (call.modelCalled) ++c.modelFrames;
    const bool carried = call.result.path == OFPS_PATH_CARRIED;
    const bool flowField = cadence && MenuFlowCheckFrame(carried, c.frames); // a failure is latched: no carry from the next present
    if (flowField) ++c.flowFrames;
    if (Dumping()) MenuEvent("core %d %d", call.modelCalled ? 1 : 0, flowField ? 1 : 0); // precedes this present's 't' line
    if (cadence && carried && !flowField) return CarriedWithoutFlow(c, hostHandle, block, list, in);
    // The reset is spent only on a frame the model ran on successfully (a creation frame, a fallback or a failed model
    // call keeps it for the next one). Mode Off's temporal full frame reports PASSTHROUGH with the model called.
    if (call.modelCalled && call.model == kNgxSuccess &&
        (call.result.path == OFPS_PATH_WARPED || carried || call.result.path == OFPS_PATH_PASSTHROUGH))
        c.resetNext = false;
    return MenuCoreFrame::Written;
}

} // namespace ofps::reshade
