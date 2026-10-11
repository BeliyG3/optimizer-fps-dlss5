// Raster FXAA: the resolve pass filtered with FXAA 3.11 (shaders/raster_fxaa.hlsli), on with Options::fxaa (--fxaa or the menu's checkbox).
// No target, descriptor or pass is added: the resolve already reads the raster targets and writes the shared outputs, so only its
// shader changes.
#include "raster.h"
#include "pipeline.h"

ID3D12PipelineState *Raster::ResolvePso(Device &d, bool fxaa)
{
    if(!fxaa) return resolvePso.Get();
    if(!fxaaResolvePso) {
        const auto resolve=Shader("raster_fxaa_resolve.cso");
        D3D12_COMPUTE_PIPELINE_STATE_DESC c{}; c.pRootSignature=computeRoot.Get(); c.CS={resolve.data(),resolve.size()};
        Check(d.gpu->CreateComputePipelineState(&c,IID_PPV_ARGS(&fxaaResolvePso)),"Create raster FXAA resolve PSO");
    }
    return fxaaResolvePso.Get();
}
void Raster::UseFxaa(Device &d)
{
    RequireCreated();
    ResolvePso(d,true);
}
