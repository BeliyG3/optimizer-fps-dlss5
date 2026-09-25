#pragma once

#include "core/frame/common.h"

namespace ofps::core {
struct FeatureState;

// Extra handles have the same lifetime gates as the host's real feature.
void RetireModelPasses(FeatureState &st, const ofps::core::gpu::GateSet &gate);
// True only on the evaluate-free creation frame.
bool PrepareModelPasses(FeatureState &st, ID3D12GraphicsCommandList *cmd, const OfpsModelInputs &inputs);
int EvaluateModelPasses(FeatureState &st, ID3D12GraphicsCommandList *cmd, const OfpsModelInputs &inputs);
} // namespace ofps::core
