// --render raster --probes: the scene pass's view of the traced probe grid (probes.h, shaders/raster_probes.hlsli PW_PROBES).
#include "raster.h"
#include <cstring>

namespace {
struct ProbeConstants { // shaders/raster_probes.hlsli, cbuffer RasterProbes (b0 space5)
    Vec3 origin; float spacing; unsigned dims[3]; float normalBias; float sideBand, padding[3];
};
static_assert(sizeof(ProbeConstants)==48);
}

void Raster::UseProbes(Device &d, const ProbeGrid &grid)
{
    RequireCreated();
    auto next=std::make_unique<Probes>(); next->grid=grid;
    // The bias and the side band in scene units (probes.h gives them in spacings).
    const ProbeConstants k{grid.origin,grid.spacing,{grid.dims[0],grid.dims[1],grid.dims[2]},ProbeNormalBias*grid.spacing,
        ProbeSideBand*grid.spacing,{}};
    next->constants=d.Buffer(256,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
    void *p=nullptr; D3D12_RANGE empty{0,0}; Check(next->constants->Map(0,&empty,&p),"Map raster probe constants");
    std::memcpy(p,&k,sizeof(k)); next->constants->Unmap(0,nullptr);
    next->table=d.Allocate(6); // the CBV, the four coefficients (t0..t3 space5), the sky visibility (t4 space5)
    D3D12_CONSTANT_BUFFER_VIEW_DESC cbv{next->constants->GetGPUVirtualAddress(),256};
    d.gpu->CreateConstantBufferView(&cbv,d.Cpu(next->table));
    for(unsigned i=0;i<5;++i) {
        D3D12_SHADER_RESOURCE_VIEW_DESC s{}; s.Format=DXGI_FORMAT_R16G16B16A16_FLOAT; s.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE3D;
        s.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; s.Texture3D.MipLevels=1;
        d.gpu->CreateShaderResourceView(i<4 ? grid.sh[i].Get() : grid.sky.Get(),&s,d.Cpu(next->table+1+i));
    }
    probes=std::move(next); // only complete: the Create*Pipelines functions and DrawScene test `probes` alone
    CreateAmbientPipelines(d);
}
