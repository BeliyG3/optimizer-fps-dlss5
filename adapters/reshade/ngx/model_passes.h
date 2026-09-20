#pragma once

#include "hook_common.h"

namespace pwhook {
struct FeatureState;

// Extra handles have the same lifetime gates as the host's real feature.
void RetireModelPasses(FeatureState &st, const pwngx::GateSet &gate);
// True only on the evaluate-free creation frame.
bool PrepareModelPasses(FeatureState &st, ID3D12GraphicsCommandList *cmd, void *params);
int EvaluateModelPasses(FeatureState &st, ID3D12GraphicsCommandList *cmd, void *params, void *callback);
} // namespace pwhook
