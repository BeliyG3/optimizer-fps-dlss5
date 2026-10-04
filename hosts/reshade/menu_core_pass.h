#pragma once
// Menu mode, stage 3: the menu frame through the core's feature (pack -> model -> unpack), so the user's layout (Mode
// Uniform/Peripheral) and temporal cadence apply to menu frames as to game frames. The feature's model host evaluates
// with menu mode's own block for this call (ModelHostNgx::BeginFrame on that block, under the feature-call lock of
// ngx_hook_api.h); the next host evaluate points it back at the host's block. The core records onto the private list,
// which runs on the private queue the core knows (menu_pipeline_gpu.cpp, correction C1). Preconditions the pipeline
// checks first (fix round 1): no setting changed since the snapshot's host evaluate (else the core would re-lay out the
// feature here), and the previous menu pass complete on the GPU (the core may retire objects during the call). The call
// never creates a model: should the core need one anyway, it is refused, a rebuild is requested for the game's next
// evaluate, and menus stay untouched until that evaluate.
//
// Task 13, the sync temporal cadence: the core runs the model every N-th menu frame and carries its edit in between.
// Menus have no game motion (the pass block's motion is zero), so the core's motion source is NVIDIA optical flow while
// a run passes frames through it: switched on at the run's first core frame, back to the game's vectors at every end of
// a run (EndRun, correction C5; the switch invalidates the core's temporal machine each time). C6: without optical flow
// no frame is ever carried; Mode Off then runs the model on every menu frame, a compressed model is blocked. A frame the
// core carried without the optical flow field (its flow failed on that frame) is never shown: it runs again as a full
// model frame in the same list, or the last output is shown again (fix round 1).
// Everything here runs under the pipeline's lock; the switch takes the core's lock (pipeline -> feature call -> core).
// The motion source and the C6 verdict: menu_core_flow.h.
#include "hosts/reshade/menu_core_flow.h"
#include "hosts/reshade/ngx_param_shim.h"
#include <d3d12.h>

namespace ofps::reshade {

void MenuCorePassRunStart(); // the next frame resets the core's history (and NR's) for a new run
void MenuCorePassHostEvaluated(); // a host evaluate of the watched feature: a model creation it owed is done by now
// A menu call met a model creation (the old model may already be retired): no menu pass of either kind, the core's or
// the direct one (final review C1), until the game's next evaluate.
bool MenuCorePassCreateBlocked();
enum class MenuCoreFrame {
    // Nothing may run (a host evaluate holds the feature, the feature is gone, the core refused the frame before
    // recording, a model would have been created, or the cadence's optical flow could not be switched on): the private
    // list must not be executed, the frame stays untouched.
    Untouched,
    Written, // the core wrote this frame's output into DLSSNR.Output
    // Fix round 1 (Codex I2): the core recorded work that must run, but its output must not be shown (a frame carried
    // without the optical flow field whose full model frame failed, or, final review, a descriptor slot the core did not
    // wait for): the pass leaves its output texture alone (MenuFrame::wrote false), so the last menu output of this run is
    // shown again; before the run's first pass the frame stays untouched.
    LastShownAgain,
};
// `block`: the pass block (MenuPassBlock) with the model pass's textures: DLSSNR.Color resting in
// NON_PIXEL_SHADER_RESOURCE, DLSSNR.Output in UNORDERED_ACCESS, the snapshot's guides. The model host writes the core's
// model inputs into it for the call and restores it.
MenuCoreFrame MenuCorePass(void *hostHandle, OwnParams &block, ID3D12GraphicsCommandList *list);

// The end of a run (EndRun): with DebugMenuDump=1 the event "cadence <frames through the core> <with the model> <with
// the optical flow field>" and a log line, then the counts restart. With DebugMenuDump=1 a frame carried without the
// field adds the event "redo <1: ran again as a full model frame, 0: the last output shown again>".
void MenuCorePassRunEnd();

} // namespace ofps::reshade
