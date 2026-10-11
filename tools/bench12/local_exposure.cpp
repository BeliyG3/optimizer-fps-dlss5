#include "local_exposure.h"
#include "pipeline.h"
#include "hlsl_compat.h"
namespace hlsl {
#include "shaders/local_exposure.hlsli"
}

namespace {
ComPtr<ID3D12Resource> Grid(Device &d, unsigned w, unsigned h, D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES hp{}; hp.Type=D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{}; desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE3D; desc.Width=w; desc.Height=h;
    desc.DepthOrArraySize=LE_SLICES; desc.MipLevels=1; desc.Format=DXGI_FORMAT_R32G32_FLOAT; desc.SampleDesc.Count=1;
    desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ComPtr<ID3D12Resource> r; Check(d.gpu->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&desc,state,nullptr,IID_PPV_ARGS(&r)),"Create local exposure grid");
    return r;
}
void Srv3d(Device &d, ID3D12Resource *r, unsigned descriptor)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC s{}; s.Format=DXGI_FORMAT_R32G32_FLOAT; s.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE3D;
    s.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; s.Texture3D.MipLevels=1;
    d.gpu->CreateShaderResourceView(r,&s,d.Cpu(descriptor));
}
void Uav3d(Device &d, ID3D12Resource *r, unsigned descriptor)
{
    D3D12_UNORDERED_ACCESS_VIEW_DESC u{}; u.Format=DXGI_FORMAT_R32G32_FLOAT; u.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE3D;
    u.Texture3D.WSize=LE_SLICES; d.gpu->CreateUnorderedAccessView(r,nullptr,&u,d.Cpu(descriptor));
}
}
void LocalExposure::Initialize(Device &d, ID3D12Resource *source, ID3D12Resource *exposure, ID3D12Resource *blurLevel,
                               unsigned presentDescriptors)
{
    sourceWidth=unsigned(source->GetDesc().Width); sourceHeight=source->GetDesc().Height;
    gridWidth=(sourceWidth+LE_CELL-1)/LE_CELL; gridHeight=(sourceHeight+LE_CELL-1)/LE_CELL;
    grid=Grid(d,gridWidth,gridHeight,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    blurred=Grid(d,gridWidth,gridHeight,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    // [0] source, [1] exposure (the build's t0, t1), [2] grid (the blur's t0 space1), [3] grid UAV, [4] blurred UAV.
    if(!descriptors) descriptors=d.Allocate(5);
    d.TextureSrv(source,descriptors); d.TextureSrv(exposure,descriptors+1); Srv3d(d,grid.Get(),descriptors+2);
    Uav3d(d,grid.Get(),descriptors+3); Uav3d(d,blurred.Get(),descriptors+4);
    Srv3d(d,blurred.Get(),presentDescriptors+4); d.TextureSrv(blurLevel,presentDescriptors+5);
    if(root) return;
    D3D12_DESCRIPTOR_RANGE inputs{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,2,0,0,0}, gridIn{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,0,1,0};
    D3D12_DESCRIPTOR_RANGE output{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,0,0,0};
    D3D12_ROOT_PARAMETER p[4]{};
    p[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; p[0].Constants={0,0,4};
    p[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; p[1].DescriptorTable={1,&inputs};
    p[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; p[2].DescriptorTable={1,&gridIn};
    p[3].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; p[3].DescriptorTable={1,&output};
    D3D12_ROOT_SIGNATURE_DESC desc{}; desc.NumParameters=4; desc.pParameters=p; root=Root(d,desc);
    auto make=[&](const char *name, ComPtr<ID3D12PipelineState> &pso) {
        auto shader=Shader(name); D3D12_COMPUTE_PIPELINE_STATE_DESC c{};
        c.pRootSignature=root.Get(); c.CS={shader.data(),shader.size()};
        Check(d.gpu->CreateComputePipelineState(&c,IID_PPV_ARGS(&pso)),"Create local exposure PSO");
    };
    make("local_exposure_grid.cso",build); make("local_exposure_blur.cso",blur);
}
void LocalExposure::Record(Device &d)
{
    const unsigned constants[4]={sourceWidth,sourceHeight,gridWidth,gridHeight};
    d.list->SetComputeRootSignature(root.Get()); d.list->SetComputeRoot32BitConstants(0,4,constants,0);
    d.list->SetComputeRootDescriptorTable(1,d.Gpu(descriptors)); d.list->SetComputeRootDescriptorTable(3,d.Gpu(descriptors+3));
    d.list->SetPipelineState(build.Get()); d.list->Dispatch(gridWidth,gridHeight,1);
    Transition(d.list.Get(),grid.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Transition(d.list.Get(),blurred.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    d.list->SetComputeRootDescriptorTable(2,d.Gpu(descriptors+2)); d.list->SetComputeRootDescriptorTable(3,d.Gpu(descriptors+4));
    d.list->SetPipelineState(blur.Get()); d.list->Dispatch((gridWidth+3)/4,(gridHeight+3)/4,LE_SLICES/4);
    Transition(d.list.Get(),blurred.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Transition(d.list.Get(),grid.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}
