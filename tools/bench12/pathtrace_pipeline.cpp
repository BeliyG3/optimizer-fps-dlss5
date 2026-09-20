#include "pathtrace.h"
#include "pipeline.h"
void Pathtrace::CreatePipelines(Device &d)
{
    D3D12_STATIC_SAMPLER_DESC sampler{}; sampler.Filter=D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.MaxLOD=D3D12_FLOAT32_MAX; sampler.ComparisonFunc=D3D12_COMPARISON_FUNC_ALWAYS; sampler.MaxAnisotropy=1;
    sampler.ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;
    // Match the shader's unbounded texture array; only allocated image indices are accessed.
    D3D12_DESCRIPTOR_RANGE srv[]={
        {D3D12_DESCRIPTOR_RANGE_TYPE_SRV,4,0,0,0},
        {D3D12_DESCRIPTOR_RANGE_TYPE_SRV,UINT_MAX,4,0,4}};
    D3D12_DESCRIPTOR_RANGE uav{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,10,0,0,0};
    D3D12_ROOT_PARAMETER p[5]{};
    p[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV; p[0].Descriptor={0,0};
    p[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV; p[1].Descriptor={0,1};
    p[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; p[2].DescriptorTable={2,srv};
    p[3].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; p[3].DescriptorTable={1,&uav};
    p[4].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV; p[4].Descriptor={1,1};
    D3D12_ROOT_SIGNATURE_DESC rd{}; rd.NumParameters=5; rd.pParameters=p; rd.NumStaticSamplers=1; rd.pStaticSamplers=&sampler;
    traceRoot=Root(d,rd); auto cs=Shader("pathtrace.cso");
    D3D12_COMPUTE_PIPELINE_STATE_DESC compute{}; compute.pRootSignature=traceRoot.Get(); compute.CS={cs.data(),cs.size()};
    Check(d.gpu->CreateComputePipelineState(&compute,IID_PPV_ARGS(&tracePso)),"Create path tracing PSO");
    D3D12_DESCRIPTOR_RANGE guides{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,12,0,0,0};
    D3D12_ROOT_PARAMETER pp[2]{}; pp[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV; pp[0].Descriptor={0,0};
    pp[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; pp[1].DescriptorTable={1,&guides}; pp[1].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
    sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    rd.NumParameters=2; rd.pParameters=pp; rd.Flags=D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    presentRoot=Root(d,rd); auto vs=Shader("present_vs.cso"), ps=Shader("present_ps.cso");
    D3D12_GRAPHICS_PIPELINE_STATE_DESC graphics{}; graphics.pRootSignature=presentRoot.Get();
    graphics.VS={vs.data(),vs.size()}; graphics.PS={ps.data(),ps.size()};
    auto &blend=graphics.BlendState.RenderTarget[0]; blend.SrcBlend=D3D12_BLEND_ONE; blend.DestBlend=D3D12_BLEND_ZERO;
    blend.BlendOp=blend.BlendOpAlpha=D3D12_BLEND_OP_ADD; blend.SrcBlendAlpha=D3D12_BLEND_ONE; blend.DestBlendAlpha=D3D12_BLEND_ZERO;
    blend.LogicOp=D3D12_LOGIC_OP_NOOP; blend.RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
    graphics.SampleMask=UINT_MAX; graphics.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID; graphics.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;
    graphics.RasterizerState.DepthClipEnable=TRUE; graphics.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_ALWAYS;
    graphics.DepthStencilState.FrontFace={D3D12_STENCIL_OP_KEEP,D3D12_STENCIL_OP_KEEP,D3D12_STENCIL_OP_KEEP,D3D12_COMPARISON_FUNC_ALWAYS};
    graphics.DepthStencilState.BackFace=graphics.DepthStencilState.FrontFace;
    graphics.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE; graphics.NumRenderTargets=1;
    graphics.RTVFormats[0]=DXGI_FORMAT_R8G8B8A8_UNORM; graphics.SampleDesc.Count=1;
    Check(d.gpu->CreateGraphicsPipelineState(&graphics,IID_PPV_ARGS(&presentPso)),"Create present PSO");
}
