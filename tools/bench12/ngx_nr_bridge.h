#pragma once
#include "device.h"

// Colour bridge around feature 18. The NR model was trained on finished SDR frames: with
// --nr-colour srgb (default) the linear HDR colour is divided by the white point (1.0), rolled off
// above 0.75 luminance and sRGB-encoded into a proxy that NR reads; the NR output region is
// decoded back to linear for presentation (the codec of the OptiScaler DLSSNR integration, after
// renodx-dlss5). --nr-colour linear hands the linear colour over as it is and only copies the region.
// Either way the presented texture is frame-size, so a padded output never reaches the display.
class NrColourBridge {
public:
    // padX/padY: the proxy is that much larger than the colour (the host's colour region at 0,0 of a
    // larger texture, as with a render region inside a full-size target); the pad stays black.
    void Configure(Device &device, ID3D12Resource *colour, ID3D12Resource *output, unsigned frameWidth,
                   unsigned frameHeight, bool encode, unsigned padX = 0, unsigned padY = 0, bool padEdge = false);
    // Records the encode; the colour returns to `state`. Returns the proxy (a UAV), or the colour
    // itself when not encoding.
    ID3D12Resource *Encode(Device &device, D3D12_RESOURCE_STATES state);
    // Records the decode (or copy) of the frame region at x,y of the output, which is a UAV before
    // and after. The presented texture ends as a pixel-shader resource.
    void Resolve(Device &device, unsigned x, unsigned y);
    ID3D12Resource *Presented() const { return presented.Get(); }
    bool Encoding() const { return encode; }
    void Reset() { proxy.Reset(); presented.Reset(); colour=output=nullptr; }
private:
    void CreatePipelines(Device &device);
    void Run(Device &device, ID3D12PipelineState *pso, unsigned table, const unsigned (&constants)[4], ID3D12Resource *target);
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> encodePso, decodePso;
    ComPtr<ID3D12Resource> proxy, presented;
    ID3D12Resource *colour=nullptr, *output=nullptr;
    unsigned descriptors=0;
    bool encode=true, padEdge=false;
};
