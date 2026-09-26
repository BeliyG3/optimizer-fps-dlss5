#pragma once
#include "core/api/ofps_core.h"
namespace ofps::core {
struct FeatureState;
bool ModelPending(FeatureState &st);
int PrepareCodec(FeatureState &st, ID3D12GraphicsCommandList *cmd, const OfpsFrameInputs &frame,
                 OfpsResource *modelColor, OfpsResource *frameBefore, bool *identity);
} // namespace ofps::core
