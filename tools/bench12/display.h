#pragma once
#include "device.h"

// Post-NGX linear pyramid, log-luminance reduction, and presentation-only exposure.
class Display {
public:
    void Initialize(Device &device, ID3D12Resource *source, unsigned presentDescriptors);
    void Prepare(Device &device, ID3D12Resource *source, bool autoExposure);
private:
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> downsample, adapt;
    std::vector<ComPtr<ID3D12Resource>> levels;
    ComPtr<ID3D12Resource> exposure;
    unsigned descriptors=0, exposureUav=0, sourceWidth=0, sourceHeight=0;
    bool initialized=false;
};
