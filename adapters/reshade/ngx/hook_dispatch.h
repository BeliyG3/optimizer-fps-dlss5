#pragma once

// The three hooked entry points of feature 18 and the state they drive: the model behind the
// host's handle (created, re-created at the layout's extent, adopted when it predates the hook)
// and the evaluate that chooses between the warp, the temporal paths and the plain pass-through.

#include "feature_state.h"

namespace pwhook {

int __cdecl HookCreate(ID3D12GraphicsCommandList *cmd, int featureId, void *params, void **outHandle);
int __cdecl HookEvaluate(ID3D12GraphicsCommandList *cmd, void *handle, void *params, void *callback);
int __cdecl HookRelease(void *handle);

} // namespace pwhook
