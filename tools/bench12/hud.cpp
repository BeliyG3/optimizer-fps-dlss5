#include "hud.h"
#include "pipeline.h"

void Hud::Create(Device &d)
{
    // The HUD blends into the RGBA16F colour, so it reads that UAV.
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support{DXGI_FORMAT_R16G16B16A16_FLOAT};
    Check(d.gpu->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT,&support,sizeof(support)),"Check HUD colour format");
    if(!(support.Support2&D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD)) throw std::runtime_error("--hud needs typed UAV loads of RGBA16F");
    const auto cs=Shader("hud.cso");
    const D3D12_DESCRIPTOR_RANGE colour{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,0,0,0}; // u0: the target's UAV
    D3D12_ROOT_PARAMETER p[2]{};
    p[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; p[0].Constants={0,0,UINT(sizeof(Constants)/4)};
    p[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; p[1].DescriptorTable={1,&colour};
    D3D12_ROOT_SIGNATURE_DESC rd{}; rd.NumParameters=UINT(std::size(p)); rd.pParameters=p;
    root=Root(d,rd);
    D3D12_COMPUTE_PIPELINE_STATE_DESC c{}; c.pRootSignature=root.Get(); c.CS={cs.data(),cs.size()};
    Check(d.gpu->CreateComputePipelineState(&c,IID_PPV_ARGS(&pso)),"Create HUD PSO");
}
void Hud::Record(Device &d, ID3D12Resource *colour, unsigned colourUav, unsigned w, unsigned h, int frame)
{
    if(!pso) Create(d);
    const auto desc=colour->GetDesc();
    if(desc.Width!=w || desc.Height!=h) throw std::logic_error("HUD size does not match the colour target");
    Constants k{}; k.layout=HudLayoutFor(w,h); k.scale=float(h)/1080; k.width=w; k.height=h;
    k.labelVisible=(frame/30)%2==0 ? 1u : 0u; // the third label blinks: 30 frames on, 30 off
    auto *list=d.list.Get();
    UavBarrier(list,colour); // after the render (and --motion none) wrote the frame
    list->SetComputeRootSignature(root.Get()); list->SetPipelineState(pso.Get());
    list->SetComputeRoot32BitConstants(0,UINT(sizeof(k)/4),&k,0);
    list->SetComputeRootDescriptorTable(1,d.Gpu(colourUav));
    list->Dispatch((w+7)/8,(h+7)/8,1);
    UavBarrier(list,colour); // NGX and NR read the frame with its HUD
}
