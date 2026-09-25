#pragma once
#include "device.h"

// --mv-format rgba16f: copies the RG16F motion guide into an RGBA16F texture (xy = motion, zw = 0),
// the layout some games hand to DLSS (Cyberpunk 2077 with Ray Reconstruction). DLSS and NR then
// receive this texture instead of the RG16F guide; the path tracer's own guide is unchanged.
// The target stays a UAV between uses, like the other NGX inputs.
class MotionPack {
public:
    void Configure(Device &device, ID3D12Resource *source, unsigned width, unsigned height);
    void Disable() { target.Reset(); }
    ID3D12Resource *Target() const { return target.Get(); }
    // Records the copy; the source must be a UAV and is returned to that state.
    ID3D12Resource *Run(Device &device);
private:
    void CreatePipeline(Device &device);
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pso;
    ComPtr<ID3D12Resource> target;
    ID3D12Resource *source=nullptr;
    unsigned descriptors=0, width=0, height=0;
};
