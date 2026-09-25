#pragma once

#include "core/api/ofps_core.h"
#include "core/temporal/machine.h"
#include <memory>

namespace ofps::core {
struct FeatureState;

struct SpreadState final : ofps::core::gpu::Disposable {
    std::unique_ptr<ofps::core::temporal::Machine> hidden[2];
    ID3D12Resource *raw = nullptr, *carried = nullptr;
    D3D12_RESOURCE_STATES rawState = D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_STATES carriedState = D3D12_RESOURCE_STATE_COMMON;
    int stages = 0, position = 0, every = 0;
    unsigned int frames = 0;
    ~SpreadState() override;
};

bool SpreadRequested(const FeatureState &st);
void RetireSpread(FeatureState &st, const ofps::core::gpu::GateSet &gate = {});
int SpreadEvaluate(FeatureState &st, ID3D12GraphicsCommandList *cmd, const OfpsModelInputs &inputs, const OfpsFrameInputs &frame);
// A single native or packed evaluate with the selected stage's own model motion.
int SpreadModel(FeatureState &st, ID3D12GraphicsCommandList *cmd, const OfpsModelInputs &inputs, const OfpsFrameInputs &frame,
                const ofps::core::temporal::FrameInputs &in, ID3D12Resource *input, ofps::core::temporal::Machine &stage);
} // namespace ofps::core
