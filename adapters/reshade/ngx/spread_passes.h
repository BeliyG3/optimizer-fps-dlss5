#pragma once

#include "ngx_temporal.h"
#include <memory>

namespace pwhook {
struct FeatureState;

struct SpreadState final : pwngx::Disposable {
    std::unique_ptr<pwtemporal::Machine> hidden[2];
    ID3D12Resource *raw = nullptr, *carried = nullptr;
    D3D12_RESOURCE_STATES rawState = D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_STATES carriedState = D3D12_RESOURCE_STATE_COMMON;
    int stages = 0, position = 0, every = 0;
    unsigned int frames = 0;
    ~SpreadState() override;
};

bool SpreadRequested(const FeatureState &st);
void RetireSpread(FeatureState &st, const pwngx::GateSet &gate = {});
int SpreadEvaluate(FeatureState &st, ID3D12GraphicsCommandList *cmd, void *params, void *callback);
// A single native or packed evaluate with the selected stage's own model motion.
int SpreadModel(FeatureState &st, ID3D12GraphicsCommandList *cmd, void *params, void *callback,
                const pwtemporal::FrameInputs &in, ID3D12Resource *input, pwtemporal::Machine &stage);
} // namespace pwhook
