#pragma once
#include <d3d12.h>

namespace ofps::core {
struct FeatureState;
struct CodecFrame;
// Retires only grid-dependent GPU objects; active temporal/background frames remain alive.
void BuryWarpGpu(FeatureState &st);
// Creation always returns NEXT_FRAME, including hosts whose models become ready immediately.
int ConfigureModelGrid(FeatureState &st, ID3D12GraphicsCommandList *cmd, const CodecFrame &codec);
int PrepareModelGridFrame(FeatureState &st, ID3D12GraphicsCommandList *cmd,
                          const OfpsFrameInputs &frame, CodecFrame &codec);
} // namespace ofps::core
