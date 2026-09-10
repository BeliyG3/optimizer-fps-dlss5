#pragma once

// Debug-only readers of what the GPU produced: the D3D12 debug layer's message queue
// ([PeripheralWarp] DebugLayer=1) and the centre-texel dump of the temporal machine
// (DebugTemporalReadback=1). Neither runs unless its key is set.

#include "feature_state.h"

namespace pwhook {

void DumpDebugLayer(ID3D12Device *device);
void TemporalDebugReadback(FeatureState &st, ID3D12GraphicsCommandList *cmd, const pwtemporal::FrameInputs &tin);

} // namespace pwhook
