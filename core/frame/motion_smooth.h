#pragma once

// Block-constant motion vectors made smooth within each surface before the model sees them.
//
// In the 32-bit kits the vectors come from the 64-bit Feed host's optical flow engine: one vector per
// grid x grid cell, expanded to the full frame by nearest cell. The model moves its history along them
// cell by cell and leaves a lattice in the picture (the DX9 bench measured it at 4 and 8 pixels). The
// pass in shaders/motion_smooth.hlsl interpolates between the cells and keeps each pixel to the cells
// of its own depth, so edges stay where they are.
//
// Off unless [PeripheralWarp] DebugMotionSmooth=1 (see diagnostics.h for why). When on, it runs only
// when the host's config says its vectors are optical flow on a grid above 1 (the tab
// publishes that through pw_ngx::SetBlockMotionGrid), and even then the shader leaves every pixel
// that is not part of a block-constant field as it is - a game's own vectors are never touched.

#include "core/api/ofps_core.h"
#include "core/frame/common.h"

#include <cstdint>

namespace ofps::core {

struct FeatureState; // owns its MotionSmooth (feature_state.h includes this header)

class MotionSmooth : public ofps::core::gpu::Disposable {
public:
    ~MotionSmooth() override;
    // Whether this object's texture fits a host motion texture of this description.
    bool Fits(const D3D12_RESOURCE_DESC &motion) const;
    // Records the pass on `cmd` and returns the smoothed texture, left in the host input state; null
    // when it cannot run (no shader beside the add-on, creation failed), and the host's vectors stay.
    ID3D12Resource *Record(ID3D12GraphicsCommandList *cmd, ID3D12Resource *motion, ID3D12Resource *depth,
                           D3D12_RESOURCE_STATES depthState, UINT depthSub, D3D12_RESOURCE_STATES motionState, UINT motionSub, DXGI_FORMAT motionView, DXGI_FORMAT depthView, int grid);

private:
    bool Create(ID3D12Device *device, const D3D12_RESOURCE_DESC &motion);

    bool failed_ = false;
    ID3D12RootSignature *rootSignature_ = nullptr;
    ID3D12PipelineState *pipeline_ = nullptr;
    ID3D12DescriptorHeap *heap_ = nullptr;
    UINT descriptorSize_ = 0;
    std::uint32_t nextSlot_ = 0;
    ID3D12Resource *output_ = nullptr;
    D3D12_RESOURCE_STATES outputState_ = D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_DESC outputDesc_{};
};

// For one evaluate: selects the host or smoothed vectors through FeatureState::frameMotion.
// Clears the borrowed pointer when the evaluate returns, whichever path it took.
class MotionSmoothScope {
public:
    MotionSmoothScope(FeatureState &st, ID3D12GraphicsCommandList *cmd, const OfpsModelInputs &inputs);
    ~MotionSmoothScope();
    MotionSmoothScope(const MotionSmoothScope &) = delete;
    MotionSmoothScope &operator=(const MotionSmoothScope &) = delete;

private:
    FeatureState &st_;
};

} // namespace ofps::core
