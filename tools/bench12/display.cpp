#include "display.h"
#include "pipeline.h"
#include <algorithm>

namespace {
constexpr auto ReadState=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE|D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
void Uav(Device &d, ID3D12Resource *resource, unsigned descriptor)
{
    D3D12_UNORDERED_ACCESS_VIEW_DESC view{}; view.Format=resource->GetDesc().Format;
    view.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
    d.gpu->CreateUnorderedAccessView(resource,nullptr,&view,d.Cpu(descriptor));
}
}
void Display::Initialize(Device &d, ID3D12Resource *source, unsigned presentDescriptors)
{
    levels.clear(); initialized=false;
    sourceWidth=unsigned(source->GetDesc().Width); sourceHeight=source->GetDesc().Height;
    unsigned w=sourceWidth,h=sourceHeight;
    do {
        w=std::max(1u,(w+1)/2); h=std::max(1u,(h+1)/2);
        levels.push_back(d.Texture(w,h,DXGI_FORMAT_R32G32B32A32_FLOAT,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,ReadState));
    } while(w>1 || h>1);
    if(!descriptors) descriptors=d.Allocate(32);
    for(unsigned i=0;i<levels.size();++i) {
        d.TextureSrv(i ? levels[i-1].Get() : source,descriptors+i*2);
        Uav(d,levels[i].Get(),descriptors+i*2+1);
    }
    // Broad copies at 1/4, 1/16, and 1/64 resolution; bilinear reconstruction
    // and the repeated box filters blur without thresholding bright emitters.
    for(unsigned i=0;i<3;++i) d.TextureSrv(levels[std::min(size_t(1+i*2),levels.size()-1)].Get(),presentDescriptors+i);
    exposure=d.Texture(1,1,DXGI_FORMAT_R32G32B32A32_FLOAT,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,ReadState);
    if(!exposureUav) exposureUav=d.Allocate(2); d.TextureSrv(levels.back().Get(),exposureUav); Uav(d,exposure.Get(),exposureUav+1);
    d.TextureSrv(exposure.Get(),presentDescriptors+3);
    D3D12_DESCRIPTOR_RANGE sr{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,0,0,0}, ur{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,0,0,0};
    D3D12_ROOT_PARAMETER parameters[3]{};
    parameters[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; parameters[0].Constants={0,0,8};
    parameters[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; parameters[1].DescriptorTable={1,&sr};
    parameters[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; parameters[2].DescriptorTable={1,&ur};
    D3D12_ROOT_SIGNATURE_DESC desc{}; desc.NumParameters=3; desc.pParameters=parameters; root=Root(d,desc);
    auto make=[&](const char *name, ComPtr<ID3D12PipelineState> &pso) {
        auto shader=Shader(name); D3D12_COMPUTE_PIPELINE_STATE_DESC p{};
        p.pRootSignature=root.Get(); p.CS={shader.data(),shader.size()};
        Check(d.gpu->CreateComputePipelineState(&p,IID_PPV_ARGS(&pso)),"Create display compute PSO");
    };
    make("display_down.cso",downsample); make("display_adapt.cso",adapt);
}
void Display::Prepare(Device &d, ID3D12Resource *source, bool autoExposure)
{
    // Source starts and ends as a pixel SRV, including NGX's distinct output.
    Transition(d.list.Get(),source,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,ReadState);
    d.list->SetComputeRootSignature(root.Get()); d.list->SetPipelineState(downsample.Get());
    unsigned w=sourceWidth,h=sourceHeight;
    for(unsigned i=0;i<levels.size();++i) {
        auto *level=levels[i].Get();
        Transition(d.list.Get(),level,ReadState,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        unsigned data[]={w,h,sourceWidth,sourceHeight,1u<<i,i==0 ? 1u : 0u,0,0};
        d.list->SetComputeRoot32BitConstants(0,8,data,0);
        d.list->SetComputeRootDescriptorTable(1,d.Gpu(descriptors+i*2));
        d.list->SetComputeRootDescriptorTable(2,d.Gpu(descriptors+i*2+1));
        w=std::max(1u,(w+1)/2); h=std::max(1u,(h+1)/2);
        d.list->Dispatch((w+7)/8,(h+7)/8,1);
        Transition(d.list.Get(),level,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,ReadState);
    }
    Transition(d.list.Get(),exposure.Get(),ReadState,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    unsigned data[]={1,1,sourceWidth,sourceHeight,1,0,initialized ? 0u : 1u,autoExposure ? 1u : 0u};
    d.list->SetPipelineState(adapt.Get()); d.list->SetComputeRoot32BitConstants(0,8,data,0);
    d.list->SetComputeRootDescriptorTable(1,d.Gpu(exposureUav));
    d.list->SetComputeRootDescriptorTable(2,d.Gpu(exposureUav+1)); d.list->Dispatch(1,1,1);
    Transition(d.list.Get(),exposure.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,ReadState);
    Transition(d.list.Get(),source,ReadState,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    initialized=true;
}
