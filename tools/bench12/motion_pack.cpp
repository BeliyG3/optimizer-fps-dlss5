#include "motion_pack.h"
#include "pipeline.h"
#include <d3dcompiler.h>

namespace {
// Compiled at startup so shaders/ stays untouched; fxc cs_5_0 is enough for a typed copy.
constexpr char PackSource[]=R"(
Texture2D<float2> motion : register(t0);
RWTexture2D<float4> packed : register(u0);
[numthreads(8,8,1)] void CS(uint2 id : SV_DispatchThreadID)
{
    uint w, h; packed.GetDimensions(w,h);
    if(id.x<w && id.y<h) packed[id]=float4(motion[id],0,0);
}
)";
}
void MotionPack::CreatePipeline(Device &d)
{
    ComPtr<ID3DBlob> code,errors;
    const HRESULT hr=D3DCompile(PackSource,sizeof(PackSource)-1,"motion_pack",nullptr,nullptr,"CS","cs_5_0",
        D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&errors);
    if(FAILED(hr) && errors) std::fprintf(stderr,"%s\n",static_cast<const char *>(errors->GetBufferPointer()));
    Check(hr,"Compile motion pack shader");
    D3D12_DESCRIPTOR_RANGE ranges[]={{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,0,0,0},{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,0,0,1}};
    D3D12_ROOT_PARAMETER table{}; table.ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; table.DescriptorTable={2,ranges};
    D3D12_ROOT_SIGNATURE_DESC desc{}; desc.NumParameters=1; desc.pParameters=&table; root=Root(d,desc);
    D3D12_COMPUTE_PIPELINE_STATE_DESC p{}; p.pRootSignature=root.Get(); p.CS={code->GetBufferPointer(),code->GetBufferSize()};
    Check(d.gpu->CreateComputePipelineState(&p,IID_PPV_ARGS(&pso)),"Create motion pack PSO");
    descriptors=d.Allocate(2);
}
void MotionPack::Configure(Device &d, ID3D12Resource *motion, unsigned w, unsigned h)
{
    if(!pso) CreatePipeline(d);
    source=motion; width=w; height=h;
    target=d.Texture(w,h,DXGI_FORMAT_R16G16B16A16_FLOAT,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    d.TextureSrv(source,descriptors);
    D3D12_UNORDERED_ACCESS_VIEW_DESC u{}; u.Format=DXGI_FORMAT_R16G16B16A16_FLOAT; u.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
    d.gpu->CreateUnorderedAccessView(target.Get(),nullptr,&u,d.Cpu(descriptors+1));
    std::printf("[info] motion vectors handed to NGX as RGBA16F %ux%u (xy motion, zw 0)\n",w,h);
}
ID3D12Resource *MotionPack::Run(Device &d)
{
    auto *list=d.list.Get();
    Transition(list,source,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    list->SetComputeRootSignature(root.Get()); list->SetPipelineState(pso.Get());
    list->SetComputeRootDescriptorTable(0,d.Gpu(descriptors));
    list->Dispatch((width+7)/8,(height+7)/8,1);
    Transition(list,source,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    UavBarrier(list,target.Get());
    return target.Get();
}
