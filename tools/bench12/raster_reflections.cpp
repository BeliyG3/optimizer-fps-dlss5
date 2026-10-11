// --sky-reflections: the sky's image-based specular in raster (RASTER_REFLECTIONS.md, shaders/raster_reflections.hlsli). The
// prefiltered cube is built once from the scene's sky panorama (shaders/sky_prefilter.hlsl), the DFG table on the CPU.
#include "raster.h"
#include "sky_reflections.h"
#include <chrono>
#include <cstdio>
#include <cstring>

namespace {
// Descriptors of the build (Raster::Reflections::table follows them): the panorama, the source cube, a UAV per source mip, a UAV
// per prefiltered mip, a null UAV (the passes' second UAV slot after the last mip).
constexpr unsigned PanoramaSrv=0, SourceSrv=1, SourceUavs=2, CubeUavs=SourceUavs+hlsl::SkySourceMips;
constexpr unsigned BuildDescriptors=CubeUavs+hlsl::SkyReflectionMips+1;

ComPtr<ID3D12Resource> CubeTexture(Device &d, unsigned size, unsigned mips)
{
    D3D12_HEAP_PROPERTIES heap{}; heap.Type=D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{}; desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D; desc.Width=size; desc.Height=size;
    desc.DepthOrArraySize=6; desc.MipLevels=UINT16(mips); desc.Format=DXGI_FORMAT_R16G16B16A16_FLOAT; desc.SampleDesc.Count=1;
    desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ComPtr<ID3D12Resource> r;
    Check(d.gpu->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,nullptr,IID_PPV_ARGS(&r)),
          "Create sky reflection cube");
    return r;
}
void CubeSrv(Device &d, ID3D12Resource *r, unsigned mips, unsigned descriptor)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC s{}; s.Format=DXGI_FORMAT_R16G16B16A16_FLOAT; s.ViewDimension=D3D12_SRV_DIMENSION_TEXTURECUBE;
    s.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; s.TextureCube.MipLevels=mips;
    d.gpu->CreateShaderResourceView(r,&s,d.Cpu(descriptor));
}
void CubeUav(Device &d, ID3D12Resource *r, unsigned mip, unsigned descriptor)
{
    D3D12_UNORDERED_ACCESS_VIEW_DESC u{}; u.Format=DXGI_FORMAT_R16G16B16A16_FLOAT; u.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
    u.Texture2DArray.MipSlice=mip; u.Texture2DArray.ArraySize=6;
    d.gpu->CreateUnorderedAccessView(r,nullptr,&u,d.Cpu(descriptor));
}
}

void Raster::UseSkyReflections(Device &d, const Scene &s)
{
    if(!created) return; // no raster shaders: a path-traced run that never draws a raster frame
    const auto start=std::chrono::steady_clock::now();
    auto next=std::make_unique<Reflections>();
    next->source=CubeTexture(d,hlsl::SkySourceSize,hlsl::SkySourceMips);
    next->cube=CubeTexture(d,hlsl::SkyReflectionSize,hlsl::SkyReflectionMips);
    const unsigned base=d.Allocate(BuildDescriptors+2);
    d.TextureSrv(s.environment.Get(),base+PanoramaSrv);
    CubeSrv(d,next->source.Get(),hlsl::SkySourceMips,base+SourceSrv);
    for(unsigned m=0;m<hlsl::SkySourceMips;++m) CubeUav(d,next->source.Get(),m,base+SourceUavs+m);
    for(unsigned m=0;m<hlsl::SkyReflectionMips;++m) CubeUav(d,next->cube.Get(),m,base+CubeUavs+m);
    D3D12_UNORDERED_ACCESS_VIEW_DESC null{}; null.Format=DXGI_FORMAT_R16G16B16A16_FLOAT; null.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
    null.Texture2DArray.ArraySize=6; d.gpu->CreateUnorderedAccessView(nullptr,nullptr,&null,d.Cpu(base+BuildDescriptors-1));
    // The scene pass's table: the prefiltered cube (t0 space8), the DFG table (t1 space8).
    next->table=base+BuildDescriptors;
    CubeSrv(d,next->cube.Get(),hlsl::SkyReflectionMips,next->table);
    const auto dfg=BuildDfgTable();
    ImageMip level{hlsl::SkyDfgSize,hlsl::SkyDfgSize,std::vector<uint8_t>(dfg.size()*sizeof(float))};
    std::memcpy(level.bytes.data(),dfg.data(),level.bytes.size());
    next->dfg=d.UploadMips(DXGI_FORMAT_R32G32_FLOAT,8,{level});
    d.TextureSrv(next->dfg.Get(),next->table+1);

    const SkyPrefilterPipelines p=CreateSkyPrefilterPipelines(d);
    d.Begin();
    auto *list=d.list.Get(); ID3D12DescriptorHeap *heaps[]={d.heap.Get()}; list->SetDescriptorHeaps(1,heaps);
    list->SetComputeRootSignature(p.root.Get()); list->SetComputeRootDescriptorTable(1,d.Gpu(base+PanoramaSrv));
    auto dispatch=[&](ID3D12PipelineState *pso, unsigned size, unsigned mip, unsigned uavs) {
        const unsigned constants[2]={size,mip};
        list->SetPipelineState(pso); list->SetComputeRoot32BitConstants(0,2,constants,0);
        list->SetComputeRootDescriptorTable(2,d.Gpu(uavs)); list->Dispatch((size+7)/8,(size+7)/8,6);
    };
    dispatch(p.source.Get(),hlsl::SkySourceSize,0,base+SourceUavs);
    for(unsigned m=1;m<hlsl::SkySourceMips;++m) { // mip m-1 at u0, mip m at u1
        UavBarrier(list,next->source.Get());
        dispatch(p.downsample.Get(),std::max(1u,hlsl::SkySourceSize>>m),m,base+SourceUavs+m-1);
    }
    Transition(list,next->source.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    for(unsigned m=0;m<hlsl::SkyReflectionMips;++m) dispatch(p.prefilter.Get(),std::max(1u,hlsl::SkyReflectionSize>>m),m,base+CubeUavs+m);
    Transition(list,next->cube.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Transition(list,next->dfg.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    d.Submit(); // waits: the build's pipelines and the source may go
    next->source.Reset();
    reflections=std::move(next);
    const double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
    std::printf("[raster] sky reflections: %u^2 cube, %u mips (roughness 0..1), %u^2 DFG table, built in %.1f ms\n",
                hlsl::SkyReflectionSize,hlsl::SkyReflectionMips,hlsl::SkyDfgSize,ms);
}
void Raster::BindSkyReflections(Device &d)
{
    if(!reflections) throw std::runtime_error("Raster frame without its sky reflection resources (Raster::UseSkyReflections)");
    d.list->SetGraphicsRootDescriptorTable(reflectionParameter,d.Gpu(reflections->table));
}
