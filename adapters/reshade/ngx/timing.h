#pragma once

// [PeripheralWarp] DebugTiming=1: a timestamp pair around the model's evaluate, resolved into the
// feature's readback ring and averaged into the status. A slot is read only when it is a full ring
// older than the current evaluate, so nothing in flight is touched.

#include "feature_state.h"

namespace pwhook {

void TimingBegin(FeatureState &st, ID3D12GraphicsCommandList *cmd);
void TimingEnd(FeatureState &st, ID3D12GraphicsCommandList *cmd);

} // namespace pwhook
