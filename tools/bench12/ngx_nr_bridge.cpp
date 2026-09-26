#include "ngx_nr_bridge.h"
#include "pipeline.h"
#include <d3dcompiler.h>

namespace {
constexpr auto Uav=D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
constexpr auto Read=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
constexpr auto Present=D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
// Compiled at startup so shaders/ stays untouched. constants: x,y = source offset, z = sRGB codec on.
constexpr char BridgeSource[]=R"(
cbuffer Constants : register(b0) { uint4 constants; };
Texture2D<float4> source : register(t0);
RWTexture2D<float4> target : register(u0);
float3 LinearToSrgb(float3 v) { v=saturate(v); return lerp(v*12.92,1.055*pow(max(v,1e-8),1.0/2.4)-0.055,step(0.0031308,v)); }
float3 SrgbToLinear(float3 v) { v=saturate(v); return lerp(v/12.92,pow((v+0.055)/1.055,2.4),step(0.04045,v)); }
[numthreads(8,8,1)] void Encode(uint2 id : SV_DispatchThreadID)
{
    uint w, h; target.GetDimensions(w,h); if(id.x>=w || id.y>=h) return;
    uint sw, sh; source.GetDimensions(sw,sh);
    // The padding of a larger proxy: black (a host's unused texels), or the edge repeated (constants.w).
    if((id.x>=sw || id.y>=sh) && constants.w==0) { target[id]=float4(0,0,0,1); return; }
    float3 c=max(source[min(id,uint2(sw,sh)-1)].rgb,0);
    if(constants.z!=0) {
        // White point 1.0; soft knee instead of a hard clip above 0.75 luminance.
        float l=dot(c,float3(0.2126,0.7152,0.0722));
        if(l>0.75) c*=(0.75+0.25*(1.0-exp(-(l-0.75)/0.25)))/l;
        c=LinearToSrgb(c);
    }
    target[id]=float4(c,1);
}
[numthreads(8,8,1)] void Decode(uint2 id : SV_DispatchThreadID)
{
    uint w, h; target.GetDimensions(w,h); if(id.x>=w || id.y>=h) return;
    float3 c=source[id+constants.xy].rgb;
    target[id]=float4(constants.z!=0 ? SrgbToLinear(c) : c,1);
}
)";
ComPtr<ID3DBlob> Compile(const char *entry)
{
    ComPtr<ID3DBlob> code,errors;
    const HRESULT hr=D3DCompile(BridgeSource,sizeof(BridgeSource)-1,"nr_bridge",nullptr,nullptr,entry,"cs_5_0",
        D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&errors);
    if(FAILED(hr) && errors) std::fprintf(stderr,"%s\n",static_cast<const char *>(errors->GetBufferPointer()));
    Check(hr,"Compile NR colour bridge"); return code;
}
void CreateUav(Device &d, ID3D12Resource *resource, unsigned descriptor)
{
    D3D12_UNORDERED_ACCESS_VIEW_DESC view{}; view.Format=resource->GetDesc().Format; view.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
    d.gpu->CreateUnorderedAccessView(resource,nullptr,&view,d.Cpu(descriptor));
}
}
void NrColourBridge::CreatePipelines(Device &d)
{
    D3D12_DESCRIPTOR_RANGE ranges[]={{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,0,0,0},{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,0,0,1}};
    D3D12_ROOT_PARAMETER p[2]{};
    p[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; p[0].Constants={0,0,4};
    p[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; p[1].DescriptorTable={2,ranges};
    D3D12_ROOT_SIGNATURE_DESC desc{}; desc.NumParameters=2; desc.pParameters=p; root=Root(d,desc);
    auto make=[&](const char *entry, ComPtr<ID3D12PipelineState> &pso) {
        auto code=Compile(entry); D3D12_COMPUTE_PIPELINE_STATE_DESC c{};
        c.pRootSignature=root.Get(); c.CS={code->GetBufferPointer(),code->GetBufferSize()};
        Check(d.gpu->CreateComputePipelineState(&c,IID_PPV_ARGS(&pso)),"Create NR colour bridge PSO");
    };
    make("Encode",encodePso); make("Decode",decodePso);
    descriptors=d.Allocate(4); // encode SRV/UAV, decode SRV/UAV
}
void NrColourBridge::Configure(Device &d, ID3D12Resource *source, ID3D12Resource *target, unsigned w, unsigned h, bool encoding,
                               unsigned padX, unsigned padY, bool edge, DXGI_FORMAT proxyFormat)
{
    if(!root) CreatePipelines(d);
    colour=source; output=target; encode=encoding; padEdge=edge;
    const auto desc=source->GetDesc();
    proxy=encode ? d.Texture(unsigned(desc.Width)+padX,desc.Height+padY,proxyFormat,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,Uav) : nullptr;
    presented=d.Texture(w,h,DXGI_FORMAT_R16G16B16A16_FLOAT,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,Present);
    if(proxy) { d.TextureSrv(colour,descriptors); CreateUav(d,proxy.Get(),descriptors+1); }
    d.TextureSrv(output,descriptors+2); CreateUav(d,presented.Get(),descriptors+3);
    std::printf("[nr] colour handed to NR as %s; presented region %ux%u\n",encode ? "an sRGB-encoded proxy (white 1.0, knee 0.75)" : "linear HDR",w,h);
}
void NrColourBridge::Run(Device &d, ID3D12PipelineState *pso, unsigned table, const unsigned (&constants)[4], ID3D12Resource *target)
{
    auto *list=d.list.Get();
    list->SetComputeRootSignature(root.Get()); list->SetPipelineState(pso);
    list->SetComputeRoot32BitConstants(0,4,constants,0); list->SetComputeRootDescriptorTable(1,d.Gpu(table));
    const auto desc=target->GetDesc();
    list->Dispatch((unsigned(desc.Width)+7)/8,(desc.Height+7)/8,1);
}
ID3D12Resource *NrColourBridge::Encode(Device &d, D3D12_RESOURCE_STATES state)
{
    if(!encode) return colour;
    auto *list=d.list.Get();
    if(state!=Read) Transition(list,colour,state,Read);
    const unsigned constants[4]={0,0,1,padEdge ? 1u : 0u};
    Run(d,encodePso.Get(),descriptors,constants,proxy.Get());
    if(state!=Read) Transition(list,colour,Read,state);
    UavBarrier(list,proxy.Get());
    return proxy.Get();
}
void NrColourBridge::Resolve(Device &d, unsigned x, unsigned y)
{
    auto *list=d.list.Get();
    Transition(list,output,Uav,Read); Transition(list,presented.Get(),Present,Uav);
    const unsigned constants[4]={x,y,encode ? 1u : 0u,0};
    Run(d,decodePso.Get(),descriptors+2,constants,presented.Get());
    Transition(list,presented.Get(),Uav,Present); Transition(list,output,Read,Uav);
}
