#pragma once
// Menu mode on the host's side, for the watched feature while menu mode is on:
//  - MenuTracedEvaluate (ModelHostNgx's evaluate, installed for every feature) hands the NR runtime a trace of the host's
//    block during the first evaluates of each generation (menu_param_book.h); with DebugMenuOwnBlock=1 it hands it
//    menu mode's own block built from this very evaluate (a check on hosts that never pause). Only the evaluates of the
//    bound model handle are traced: the core's extra model passes evaluate other handles with the same block;
//  - MenuSnapshotHostEvaluate, after a successful host evaluate, snapshots the parameters and the depth of that same
//    evaluate and publishes them together (one generation: requirement 5 of the spike verdict);
//  - MenuExitReset: the first host evaluate after menu passes runs on a wrapper that answers DLSSNR.Reset=1 (C8).
#include "hosts/reshade/menu_param_book.h"

namespace ofps::reshade {

MenuParamBook &MenuBook();
void MenuSetEvaluatingHost(void *hostHandle); // this thread's host evaluate of the watched feature (null: none)
int MenuTracedEvaluate(ID3D12GraphicsCommandList *cmd, void *handle, void *params, void *callback);
// The host block's colour/output and the extent the core created the model at. False without colour or output.
bool MenuHostShapeOf(void *params, MenuShape *out);
// After a successful host evaluate of the watched feature, bound as `tag` (MenuParamBook::BindIfNoRelease). Null:
// published, or the previous pair kept (the generation's first evaluates are still traced, a pass still reads the other
// guide set, or the book was re-bound meanwhile); else why this feature cannot run in menus, for the tab (the newest
// snapshot of `tag`, and only of `tag`, is then withdrawn).
// settingsEpoch: MenuSettingsEpoch() read before the host evaluate's core call (the settings that evaluate applied).
const char *MenuSnapshotHostEvaluate(ID3D12GraphicsCommandList *hostList, void *params, const MenuTag &tag,
                                     std::uint64_t settingsEpoch);

// The first host evaluate after menu passes (ruling 2026-09-29): a ResetParams (ngx_param_shim.h) around the host's
// block that answers DLSSNR.Reset=1. The shell hands Params() to the core's frame reading (ReadFrameInputs: the core
// sees hostReset=1 and drops its temporal/spread history) and to ModelHostNgx::BeginFrame (the model reads Reset=1, and
// whatever the model host writes for Reset stays in the wrapper). The host's block is never given a Reset key: its
// presence, value and result code are unchanged (C8). Alive until the core's evaluate returned.
class MenuExitReset {
public:
    explicit MenuExitReset(void *params);
    NVSDK_NGX_Parameter *Params() { return &wrapper_; }
    MenuExitReset(const MenuExitReset &) = delete;
    MenuExitReset &operator=(const MenuExitReset &) = delete;

private:
    ResetParams wrapper_;
};

} // namespace ofps::reshade
