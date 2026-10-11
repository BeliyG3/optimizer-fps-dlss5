// --render raster: --cull-check's occlusion queries, --cull-debug's overlay and the statistics of the small-instance occlusion
// (raster_occlusion.h).
#include "raster_occlusion.h"
#include "pipeline.h"

namespace {
const D3D12_INPUT_ELEMENT_DESC InstanceInput{"INSTANCE",0,DXGI_FORMAT_R32_UINT,0,0,D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA,1};
// The scene pass's vertex shader (bit-identical depth), on the scene pass's root, as raster_pipeline.cpp's GraphicsDesc.
D3D12_GRAPHICS_PIPELINE_STATE_DESC Desc(ID3D12RootSignature *root, const std::vector<char> &vs, const std::vector<char> &ps)
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC g{}; g.pRootSignature=root; g.InputLayout={&InstanceInput,1};
    g.VS={vs.data(),vs.size()}; g.PS={ps.data(),ps.size()};
    auto &blend=g.BlendState.RenderTarget[0]; blend.SrcBlend=D3D12_BLEND_ONE; blend.DestBlend=D3D12_BLEND_ZERO;
    blend.BlendOp=blend.BlendOpAlpha=D3D12_BLEND_OP_ADD; blend.SrcBlendAlpha=D3D12_BLEND_ONE; blend.DestBlendAlpha=D3D12_BLEND_ZERO;
    blend.LogicOp=D3D12_LOGIC_OP_NOOP; blend.RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
    g.SampleMask=UINT_MAX; g.SampleDesc.Count=1;
    g.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID; g.RasterizerState.CullMode=D3D12_CULL_MODE_NONE; g.RasterizerState.DepthClipEnable=TRUE;
    g.DepthStencilState.FrontFace={D3D12_STENCIL_OP_KEEP,D3D12_STENCIL_OP_KEEP,D3D12_STENCIL_OP_KEEP,D3D12_COMPARISON_FUNC_ALWAYS};
    g.DepthStencilState.BackFace=g.DepthStencilState.FrontFace;
    g.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE; g.DSVFormat=DXGI_FORMAT_D32_FLOAT;
    return g;
}
}

ID3D12PipelineState *RasterOcclusion::CheckPso(Device &d, ID3D12RootSignature *sceneRoot, bool reverse)
{
    for(auto &entry:checkPsos) if(entry.root==sceneRoot && entry.reverse==reverse) return entry.pso.Get();
    // Depth EQUAL against the finished depth buffer, no writes: only the instance's own visible pixels pass.
    const auto vs=Shader("raster_vs.cso"), ps=Shader("raster_tubes_prepass_ps.cso"); // alive until the PSO exists
    auto g=Desc(sceneRoot,vs,ps);
    g.DepthStencilState.DepthEnable=TRUE; g.DepthStencilState.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ZERO;
    g.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_EQUAL;
    RootPso entry{sceneRoot,reverse,nullptr};
    Check(d.gpu->CreateGraphicsPipelineState(&g,IID_PPV_ARGS(&entry.pso)),"Create cull check PSO");
    checkPsos.push_back(entry); return checkPsos.back().pso.Get();
}
ID3D12PipelineState *RasterOcclusion::DebugPso(Device &d, ID3D12RootSignature *sceneRoot)
{
    for(auto &entry:debugPsos) if(entry.root==sceneRoot) return entry.pso.Get();
    // No depth test; the blend multiplies the colour below by the shader's red (shaders/raster_prepass.hlsl CullDebug).
    const auto vs=Shader("raster_vs.cso"), ps=Shader("raster_cull_debug_ps.cso");
    auto g=Desc(sceneRoot,vs,ps);
    g.NumRenderTargets=1; g.RTVFormats[0]=DXGI_FORMAT_R16G16B16A16_FLOAT; g.DSVFormat=DXGI_FORMAT_UNKNOWN;
    auto &blend=g.BlendState.RenderTarget[0]; blend.BlendEnable=TRUE; blend.SrcBlend=D3D12_BLEND_ZERO; blend.DestBlend=D3D12_BLEND_SRC_COLOR;
    blend.SrcBlendAlpha=D3D12_BLEND_ZERO; blend.DestBlendAlpha=D3D12_BLEND_ONE;
    RootPso entry{sceneRoot,false,nullptr};
    Check(d.gpu->CreateGraphicsPipelineState(&g,IID_PPV_ARGS(&entry.pso)),"Create cull debug PSO");
    debugPsos.push_back(entry); return debugPsos.back().pso.Get();
}
void RasterOcclusion::RecordCheck(Device &d, const Scene &s, const RasterInstances::List &camera, ID3D12RootSignature *sceneRoot,
                                  D3D12_CPU_DESCRIPTOR_HANDLE dsv, bool reverse, const RasterInstances &lists)
{
    if(!check || !recorded || !candidates || !queries) return;
    auto *list=d.list.Get();
    list->SetPipelineState(CheckPso(d,sceneRoot,reverse));
    list->OMSetRenderTargets(0,nullptr,FALSE,&dsv);
    lists.Bind(list);
    unsigned query=0;
    for(const auto &run:camera.smallRuns) for(unsigned k=0;k<run.count;++k,++query) {
        list->BeginQuery(queries.Get(),D3D12_QUERY_TYPE_OCCLUSION,query);
        list->DrawInstanced(s.instancedMeshes[run.mesh].triangles*3,1,0,run.first+k);
        list->EndQuery(queries.Get(),D3D12_QUERY_TYPE_OCCLUSION,query);
    }
    list->ResolveQueryData(queries.Get(),D3D12_QUERY_TYPE_OCCLUSION,0,query,readback.Get(),UINT64(maxRuns)*2*16+UINT64(smallInstances)*4);
    pendingCheck=true;
}
void RasterOcclusion::RecordDebug(Device &d, ID3D12RootSignature *sceneRoot, D3D12_CPU_DESCRIPTOR_HANDLE colour, const RasterInstances &lists)
{
    if(!debug || !recorded || !candidates) return;
    auto *list=d.list.Get();
    list->SetPipelineState(DebugPso(d,sceneRoot));
    list->OMSetRenderTargets(1,&colour,FALSE,nullptr);
    list->IASetVertexBuffers(0,1,&visibleView);
    list->ExecuteIndirect(drawSignature.Get(),runs,args.Get(),UINT64(runs)*16,nullptr,0);
    lists.Bind(list);
}
void RasterOcclusion::CollectCheck(const Scene &s)
{
    pendingCheck=false;
    const size_t argsBytes=size_t(maxRuns)*2*16, flagBytes=size_t(smallInstances)*4;
    void *data=nullptr; D3D12_RANGE range{0,argsBytes+flagBytes+size_t(smallInstances)*8};
    Check(readback->Map(0,&range,&data),"Map cull check readback");
    const auto *bytes=static_cast<const unsigned char *>(data);
    const auto *flagged=reinterpret_cast<const unsigned *>(bytes+argsBytes);
    const auto *samples=reinterpret_cast<const UINT64 *>(bytes+argsBytes+flagBytes);
    unsigned culled=0, wrong=0;
    for(size_t k=0;k<lastCandidates.size();++k) {
        if(!flagged[k]) continue;
        ++culled;
        if(!samples[k]) continue;
        ++wrong;
        const unsigned i=lastCandidates[k].instance; const auto &box=s.instanceBoxes[i];
        if(wrong<=10) std::printf("[cull-check] culled but visible: instance %u (box %.2f %.2f %.2f .. %.2f %.2f %.2f), %llu samples\n",i,
                                  box.lo.x,box.lo.y,box.lo.z,box.hi.x,box.hi.y,box.hi.z,(unsigned long long)samples[k]);
    }
    D3D12_RANGE none{0,0}; readback->Unmap(0,&none);
    ++checkedFrames; checkedCulled+=culled; violations+=wrong;
}
void RasterOcclusion::Report() const
{
    if(frames) std::printf("[cull] occlusion: %.1f small candidates in the camera's view per frame, %.1f occluded (%.1f %%), %u frames\n",
        candidateTotal/frames,occludedTotal/frames,candidateTotal>0 ? 100*occludedTotal/candidateTotal : 0.0,frames);
    if(checkedFrames) std::printf("[cull-check] %u frames: %u culled candidates tested, %u culled but visible\n",checkedFrames,checkedCulled,violations);
}
