#pragma once

#include "core/frame/common.h"

#include <cstdint>

namespace ofps::core {
struct FeatureState;

// Extra handles have the same lifetime gates as the host's real feature.
void RetireModelPasses(FeatureState &st, const ofps::core::gpu::GateSet &gate);
// The NR history rule. Every feature (the host's real one, each extra pass) remembers the frame (Ctx().evalCounter)
// of its last successful evaluate (FeatureState::histories); every evaluate supplies motion that reaches back to an
// anchor frame. A feature whose history does not end on that anchor, or has none on record, is evaluated with Reset.
// The anchor of the host's own one-frame vectors is this feature's previous evaluate. A caller that hands the model
// an accumulated chain names the chain's anchor (Machine::ChainFrame / PendingFrame) with this before the evaluate.
void SetMotionAnchor(FeatureState &st, std::uint64_t frame);
// True only on the evaluate-free creation frame.
bool PrepareModelPasses(FeatureState &st, ID3D12GraphicsCommandList *cmd, const OfpsModelInputs &inputs);
int EvaluateModelPasses(FeatureState &st, ID3D12GraphicsCommandList *cmd, const OfpsModelInputs &inputs);
} // namespace ofps::core
