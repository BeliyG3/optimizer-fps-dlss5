#pragma once
#include "device.h"

// --local-exposure (shaders/local_exposure.hlsl and .hlsli): the bilateral grid of the display source's log luminance
// after the auto exposure, built and blurred each frame after the meter (Display::Prepare). The present pass slices the
// blurred grid (its SRV at presentDescriptors+4) and reads the wide blur from a coarse pyramid level (+5, alpha: the
// mean natural-log luminance before exposure). Created with the display, so the present table's views always exist.
class LocalExposure {
public:
    void Initialize(Device &device, ID3D12Resource *source, ID3D12Resource *exposure, ID3D12Resource *blurLevel,
                    unsigned presentDescriptors);
    void Record(Device &device); // the source and exposure as pixel / non-pixel shader resources
private:
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> build, blur;
    ComPtr<ID3D12Resource> grid, blurred;
    unsigned descriptors=0, sourceWidth=0, sourceHeight=0, gridWidth=0, gridHeight=0;
};
// The pyramid level used as the local exposure's wide blur: the first at most 8 texels wide (1/8 of the frame or more
// per texel, bilinear), UE's "blurred luminance" of a large kernel.
inline size_t LocalExposureBlurLevel(const std::vector<unsigned> &widths)
{
    for(size_t i=0;i<widths.size();++i) if(widths[i]<=8) return i;
    return widths.empty() ? 0 : widths.size()-1;
}
