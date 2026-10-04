#pragma once
// Guide snapshots of the watched feature for the menu passes. The host thread copies the host's depth, on the host's own
// list after its evaluate, into one of two sets; motion is a zero texture (menus carry no game motion). A set reserved by
// a recording pass, read by an unfinished one, or whose previous copy is still in flight on a host list, is never
// rewritten (skipped, never waited on). Each copy is tracked like a readable-guide twin (readable_guides.h hold: from
// the recorded copy until the host list was submitted and passed a fence signalled after it). Replaced sets and the
// sets of a released feature go to the core's graveyard, gated on the last pass that read them (f2) and on the core's
// registered queues; the hold keeps its own reference until the host's copy passed. Set ids carry an epoch: a snapshot
// never gets another epoch's set.
#include <d3d12.h>

namespace ofps::reshade {

constexpr int kMenuGuidesBusy = -1, kMenuGuidesRefused = -2;
// Host thread: copies the block's depth into a free set. Returns the set id, kMenuGuidesBusy, or kMenuGuidesRefused with
// *problem saying why (no depth or motion, a depth-stencil depth, textures that could not be created).
int MenuGuidesCopy(ID3D12GraphicsCommandList *hostList, void *params, const char **problem);
// Present thread: the textures of set `set` for one pass, reserved until MenuGuidesUsed (submitted: read until that f2
// value) or MenuGuidesUnused. False: that set is gone.
bool MenuGuidesFor(int set, ID3D12Resource **depth, ID3D12Resource **motion);
void MenuGuidesUsed(ID3D12Fence *f2, UINT64 value2);
void MenuGuidesUnused();
void MenuGuidesRelease(); // the watched feature was released

} // namespace ofps::reshade
