#include "raster.h"
#include "pipeline.h"
#include <string>

namespace {
D3D12_STATIC_SAMPLER_DESC LinearWrap() // s0, as the path tracer's
{
    D3D12_STATIC_SAMPLER_DESC s{}; s.Filter=D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    s.AddressU=s.AddressV=s.AddressW=D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    s.MaxLOD=D3D12_FLOAT32_MAX; s.ComparisonFunc=D3D12_COMPARISON_FUNC_ALWAYS; s.MaxAnisotropy=1;
    s.ShaderRegister=0; s.ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;
    return s;
}
D3D12_STATIC_SAMPLER_DESC ShadowCompare() // s1: bilinear PCF; outside the map is lit
{
    D3D12_STATIC_SAMPLER_DESC s{}; s.Filter=D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    s.AddressU=s.AddressV=s.AddressW=D3D12_TEXTURE_ADDRESS_MODE_BORDER; s.BorderColor=D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    s.ComparisonFunc=D3D12_COMPARISON_FUNC_LESS_EQUAL; s.MaxAnisotropy=1; s.MaxLOD=0;
    s.ShaderRegister=1; s.ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;
    return s;
}
D3D12_STATIC_SAMPLER_DESC LinearClamp() // s2, --lightmap's atlases: bilinear inside the atlas, never across its edges
{
    D3D12_STATIC_SAMPLER_DESC s=LinearWrap();
    s.AddressU=s.AddressV=s.AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP; s.MaxLOD=0; s.ShaderRegister=2;
    return s;
}
D3D12_STATIC_SAMPLER_DESC ReflectionClamp() // s3, --sky-reflections: the prefiltered cube's mips and the DFG table, trilinear, clamped
{
    D3D12_STATIC_SAMPLER_DESC s=LinearWrap();
    s.AddressU=s.AddressV=s.AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP; s.ShaderRegister=3;
    return s;
}
D3D12_ROOT_PARAMETER Cbv(unsigned reg, unsigned space=0)
{
    D3D12_ROOT_PARAMETER p{}; p.ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV; p.Descriptor={reg,space}; return p;
}
D3D12_ROOT_PARAMETER Srv(unsigned reg, unsigned space, D3D12_SHADER_VISIBILITY visibility)
{
    D3D12_ROOT_PARAMETER p{}; p.ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV; p.Descriptor={reg,space}; p.ShaderVisibility=visibility; return p;
}
D3D12_ROOT_PARAMETER Table(const D3D12_DESCRIPTOR_RANGE *ranges, unsigned count, D3D12_SHADER_VISIBILITY visibility)
{
    D3D12_ROOT_PARAMETER p{}; p.ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    p.DescriptorTable={count,ranges}; p.ShaderVisibility=visibility; return p;
}
D3D12_ROOT_PARAMETER RootConstants(unsigned reg, unsigned count, D3D12_SHADER_VISIBILITY visibility, unsigned space=0)
{
    D3D12_ROOT_PARAMETER p{}; p.ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    p.Constants={reg,space,count}; p.ShaderVisibility=visibility; return p;
}
// The scene table at Scene::srvBase, as the path tracer binds it: t0..t4, then the unbounded textures from t5.
const D3D12_DESCRIPTOR_RANGE SceneTable[]={{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,5,0,0,0},{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,UINT_MAX,5,0,5}};
const D3D12_DESCRIPTOR_RANGE ShadowTable{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,1,2,0};         // t1 space2
const D3D12_DESCRIPTOR_RANGE DynamicShadowTable{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,1,3,0};  // t1 space3 (--lightmap)
const D3D12_DESCRIPTOR_RANGE AtlasTable{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,UINT_MAX,0,4,0};   // t0 space4 on (--lightmap)
const D3D12_DESCRIPTOR_RANGE CascadeTable{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,3,2,0};        // t3 space2 (--csm)
const D3D12_DESCRIPTOR_RANGE AoTable{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,4,2,0};             // t4 space2 (--ao)
// The scene and shadow passes: frame and raster constants, the scene table, lamp groups, raster lights, the shadow map.
std::vector<D3D12_ROOT_PARAMETER> GraphicsParameters()
{
    return {Cbv(0),Cbv(1),Table(SceneTable,2,D3D12_SHADER_VISIBILITY_ALL),Srv(1,1,D3D12_SHADER_VISIBILITY_PIXEL),
        Srv(0,2,D3D12_SHADER_VISIBILITY_PIXEL),Table(&ShadowTable,1,D3D12_SHADER_VISIBILITY_PIXEL)};
}
// --lightmap: graphicsRoot's parameters, then 6 the lightmap UVs (t0 space3, read by the vertex shader), 7 the dynamic
// shadow map (t1 space3; the dynamic cascade array with --csm), 8 the atlases (t0 space4 on), 9 scaleFull, scaleSun and
// the rest flag (b2), 10 Scene::unbakedTriangles (t2 space3, the list shadow draws' vertex shader); sampler s2.
// shaders/raster_lightmap.hlsli, raster_shadow.hlsl.
std::vector<D3D12_ROOT_PARAMETER> BakedParameters()
{
    auto p=GraphicsParameters();
    p.push_back(Srv(0,3,D3D12_SHADER_VISIBILITY_VERTEX)); p.push_back(Table(&DynamicShadowTable,1,D3D12_SHADER_VISIBILITY_PIXEL));
    p.push_back(Table(&AtlasTable,1,D3D12_SHADER_VISIBILITY_PIXEL)); p.push_back(RootConstants(2,3,D3D12_SHADER_VISIBILITY_PIXEL));
    p.push_back(Srv(2,3,D3D12_SHADER_VISIBILITY_VERTEX)); // Raster::BakedTriangleList
    return p;
}
// --csm, after the scene root's parameters (graphicsRoot's, or the baked root's with --lightmap): the cascades
// (b2 space1), the cascade array (t3 space2) and the shadow pass's cascade index (b3 space1).
// shaders/raster_csm.hlsli, raster_shadow.hlsl.
void AddCsmParameters(std::vector<D3D12_ROOT_PARAMETER> &p)
{
    p.push_back(Cbv(2,1)); p.push_back(Table(&CascadeTable,1,D3D12_SHADER_VISIBILITY_PIXEL));
    p.push_back(RootConstants(3,1,D3D12_SHADER_VISIBILITY_VERTEX,1));
}
// --probes: the constants (b0 space5) and the five probe textures (t0..t4 space5), shaders/raster_probes.hlsli.
const D3D12_DESCRIPTOR_RANGE ProbeTable[]={{D3D12_DESCRIPTOR_RANGE_TYPE_CBV,1,0,5,0},{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,5,0,5,1}};
// --lamp-shadows: the maps (t1, t2 space7), shaders/raster_lamp_shadow.hlsli.
const D3D12_DESCRIPTOR_RANGE LampShadowTable{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,2,1,7,0};
// --sky-reflections: the prefiltered cube and the DFG table (t0, t1 space8), shaders/raster_reflections.hlsli.
const D3D12_DESCRIPTOR_RANGE ReflectionTable{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,2,0,8,0};
// The scene pass's pixel shader of a run: raster[_lampshadow][_tubes][_probes][_sky][_ao][_csm][_lightmap]_ps.cso (build.cmd),
// `sky` for --raster-sky-ambient, `tubes` for --lamp-tubes, `lamp` for --lamp-shadows.
std::vector<char> ScenePixelShader(bool lamp, bool tubes, bool probes, bool sky, bool ao, bool csm, bool baked)
{
    std::string name="raster_";
    if(lamp) name+="lampshadow_";
    if(tubes) name+="tubes_";
    if(probes) name+="probes_";
    if(sky) name+="sky_";
    if(ao) name+="ao_";
    if(csm) name+="csm_";
    if(baked) name+="lightmap_";
    return Shader((name+"ps.cso").c_str());
}
// s0, s1 and s3; s2 too for the baked root (atlasSampler). The input assembler feeds the per-instance entry (InstanceInput).
ComPtr<ID3D12RootSignature> GraphicsRoot(Device &d, const std::vector<D3D12_ROOT_PARAMETER> &p, bool atlasSampler)
{
    std::vector<D3D12_STATIC_SAMPLER_DESC> samplers={LinearWrap(),ShadowCompare(),ReflectionClamp()};
    if(atlasSampler) samplers.push_back(LinearClamp());
    D3D12_ROOT_SIGNATURE_DESC rd{}; rd.NumParameters=UINT(p.size()); rd.pParameters=p.data();
    rd.Flags=D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    rd.NumStaticSamplers=UINT(samplers.size()); rd.pStaticSamplers=samplers.data();
    return Root(d,rd);
}
// Every scene geometry draw's one vertex input: `uint instance : INSTANCE`, per instance from slot 0 (raster_instances.h), so
// StartInstanceLocation picks the draw's run of visible instances. The vertices themselves are pulled from the scene buffers.
const D3D12_INPUT_ELEMENT_DESC InstanceInput{"INSTANCE",0,DXGI_FORMAT_R32_UINT,0,0,D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA,1};
D3D12_GRAPHICS_PIPELINE_STATE_DESC GraphicsDesc(ID3D12RootSignature *root, const std::vector<char> &vs, const std::vector<char> &ps)
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC g{}; g.pRootSignature=root; g.InputLayout={&InstanceInput,1};
    g.VS={vs.data(),vs.size()}; g.PS={ps.data(),ps.size()};
    auto &blend=g.BlendState.RenderTarget[0]; blend.SrcBlend=D3D12_BLEND_ONE; blend.DestBlend=D3D12_BLEND_ZERO;
    blend.BlendOp=blend.BlendOpAlpha=D3D12_BLEND_OP_ADD; blend.SrcBlendAlpha=D3D12_BLEND_ONE; blend.DestBlendAlpha=D3D12_BLEND_ZERO;
    blend.LogicOp=D3D12_LOGIC_OP_NOOP; blend.RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
    g.SampleMask=UINT_MAX; g.SampleDesc.Count=1;
    // The trace treats triangles as two-sided, so nothing is culled.
    g.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID; g.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;
    g.RasterizerState.DepthClipEnable=TRUE;
    g.DepthStencilState.DepthEnable=TRUE; g.DepthStencilState.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ALL;
    g.DepthStencilState.FrontFace={D3D12_STENCIL_OP_KEEP,D3D12_STENCIL_OP_KEEP,D3D12_STENCIL_OP_KEEP,D3D12_COMPARISON_FUNC_ALWAYS};
    g.DepthStencilState.BackFace=g.DepthStencilState.FrontFace;
    g.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE; g.DSVFormat=DXGI_FORMAT_D32_FLOAT;
    return g;
}
ComPtr<ID3D12PipelineState> ShadowPso(Device &d, ID3D12RootSignature *root, const std::vector<char> &vs, const std::vector<char> &ps)
{
    auto g=GraphicsDesc(root,vs,ps);
    g.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_LESS; g.RasterizerState.SlopeScaledDepthBias=1.5f;
    ComPtr<ID3D12PipelineState> pso; Check(d.gpu->CreateGraphicsPipelineState(&g,IID_PPV_ARGS(&pso)),"Create raster shadow PSO");
    return pso;
}
// The scene pass writes colour (RGBA16F) and motion (RG16F): pso[0] standard depth, pso[1] reverse depth (cleared to 0).
// afterPrepass (--ao): both test EQUAL to the depth the prepass drew and do not write it.
void ScenePsos(Device &d, ID3D12RootSignature *root, const std::vector<char> &vs, const std::vector<char> &ps, ComPtr<ID3D12PipelineState> (&pso)[2],
               bool afterPrepass=false)
{
    auto g=GraphicsDesc(root,vs,ps); g.NumRenderTargets=2;
    g.RTVFormats[0]=DXGI_FORMAT_R16G16B16A16_FLOAT; g.RTVFormats[1]=DXGI_FORMAT_R16G16_FLOAT;
    if(afterPrepass) g.DepthStencilState.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ZERO;
    g.DepthStencilState.DepthFunc=afterPrepass ? D3D12_COMPARISON_FUNC_EQUAL : D3D12_COMPARISON_FUNC_LESS;
    Check(d.gpu->CreateGraphicsPipelineState(&g,IID_PPV_ARGS(&pso[0])),"Create raster scene PSO");
    g.DepthStencilState.DepthFunc=afterPrepass ? D3D12_COMPARISON_FUNC_EQUAL : D3D12_COMPARISON_FUNC_GREATER;
    Check(d.gpu->CreateGraphicsPipelineState(&g,IID_PPV_ARGS(&pso[1])),"Create raster reverse-depth scene PSO");
}
}

void Raster::Create(Device &d)
{
    std::vector<char> vs,ps,shadowVs,shadowPs,resolve,zero;
    try {
        vs=Shader("raster_vs.cso"); ps=Shader("raster_ps.cso");
        shadowVs=Shader("raster_shadow_vs.cso"); shadowPs=Shader("raster_shadow_ps.cso");
        resolve=Shader("raster_resolve.cso"); zero=Shader("raster_zero_motion.cso");
    } catch(const std::exception &failure) { error=failure.what(); return; }
    const D3D12_DESCRIPTOR_RANGE targets{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,3,2,2,0};       // t2..t4 space2: colour, depth, motion
    const D3D12_DESCRIPTOR_RANGE outputs{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,10,0,0,0};      // the trace's u0..u9
    {
        auto p=GraphicsParameters();
        AddAmbientParameters(p); // nothing is in use yet but --sky-reflections' table, which every scene root has
        graphicsRoot=GraphicsRoot(d,p,false);
    }
    {
        // The resolve: frame and raster constants, the scene table (the sky), the trace's outputs, the raster targets.
        const D3D12_ROOT_PARAMETER p[]={Cbv(0),Cbv(1),Table(SceneTable,2,D3D12_SHADER_VISIBILITY_ALL),Table(&outputs,1,D3D12_SHADER_VISIBILITY_ALL),
            Table(&targets,1,D3D12_SHADER_VISIBILITY_ALL)};
        const D3D12_STATIC_SAMPLER_DESC sampler=LinearWrap(); // s0 only
        D3D12_ROOT_SIGNATURE_DESC rd{}; rd.NumParameters=UINT(std::size(p)); rd.pParameters=p;
        rd.NumStaticSamplers=1; rd.pStaticSamplers=&sampler;
        computeRoot=Root(d,rd);
    }
    shadowPso=ShadowPso(d,graphicsRoot.Get(),shadowVs,shadowPs);
    ScenePsos(d,graphicsRoot.Get(),vs,ps,scenePso);
    D3D12_COMPUTE_PIPELINE_STATE_DESC c{}; c.pRootSignature=computeRoot.Get(); c.CS={resolve.data(),resolve.size()};
    Check(d.gpu->CreateComputePipelineState(&c,IID_PPV_ARGS(&resolvePso)),"Create raster resolve PSO");
    c.CS={zero.data(),zero.size()};
    Check(d.gpu->CreateComputePipelineState(&c,IID_PPV_ARGS(&zeroPso)),"Create zero-motion PSO");
    CreateDescriptors(d);
    created=true;
}
// --raster-sky-ambient's CBV (b4 space1, shaders/raster_common.hlsli), then --probes' table, then --lamp-tubes' lines (t0 space6)
// and their count (one root constant, b0 space6; shaders/raster_tubes.hlsli), then --lamp-shadows' three, then --sky-reflections'
// table (always: every scene pixel shader reads it): the last parameters of every scene root (after the baked and cascade ones,
// before --ao's table). Records their indices.
void Raster::AddAmbientParameters(std::vector<D3D12_ROOT_PARAMETER> &p)
{
    if(sky) { p.push_back(Cbv(4,1)); skyParameter=unsigned(p.size()-1); }
    if(probes) { p.push_back(Table(ProbeTable,2,D3D12_SHADER_VISIBILITY_PIXEL)); probeParameter=unsigned(p.size()-1); }
    if(tubes) {
        tubeParameter=unsigned(p.size());
        p.push_back(Srv(0,6,D3D12_SHADER_VISIBILITY_PIXEL)); p.push_back(RootConstants(0,1,D3D12_SHADER_VISIBILITY_PIXEL,6));
    }
    if(lampShadows) { // the maps' table, the constants (b0 space7), the light slices (t3 space7): Raster::BindLampShadows
        lampShadowParameter=unsigned(p.size());
        p.push_back(Table(&LampShadowTable,1,D3D12_SHADER_VISIBILITY_PIXEL)); p.push_back(Cbv(0,7)); p.push_back(Srv(3,7,D3D12_SHADER_VISIBILITY_PIXEL));
    }
    p.push_back(Table(&ReflectionTable,1,D3D12_SHADER_VISIBILITY_PIXEL)); reflectionParameter=unsigned(p.size()-1);
}
// --sky-reflections' build (shaders/sky_prefilter.hlsl), used once by UseSkyReflections: 0 the pass's size and mip (b0), 1 the
// panorama and the source cube (t0, t1), 2 two UAVs (u0, u1); s0 as the path tracer's.
Raster::SkyPrefilterPipelines Raster::CreateSkyPrefilterPipelines(Device &d)
{
    const D3D12_DESCRIPTOR_RANGE sources{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,2,0,0,0}, targets{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,2,0,0,0};
    const D3D12_ROOT_PARAMETER p[]={RootConstants(0,2,D3D12_SHADER_VISIBILITY_ALL),Table(&sources,1,D3D12_SHADER_VISIBILITY_ALL),
        Table(&targets,1,D3D12_SHADER_VISIBILITY_ALL)};
    const D3D12_STATIC_SAMPLER_DESC sampler=LinearWrap();
    D3D12_ROOT_SIGNATURE_DESC rd{}; rd.NumParameters=UINT(std::size(p)); rd.pParameters=p; rd.NumStaticSamplers=1; rd.pStaticSamplers=&sampler;
    SkyPrefilterPipelines result; result.root=Root(d,rd);
    auto pso=[&](const char *file) {
        const auto cs=Shader(file); ComPtr<ID3D12PipelineState> state;
        D3D12_COMPUTE_PIPELINE_STATE_DESC c{}; c.pRootSignature=result.root.Get(); c.CS={cs.data(),cs.size()};
        Check(d.gpu->CreateComputePipelineState(&c,IID_PPV_ARGS(&state)),"Create sky prefilter PSO");
        return state;
    };
    result.source=pso("sky_source.cso"); result.downsample=pso("sky_downsample.cso"); result.prefilter=pso("sky_prefilter.cso");
    return result;
}
void Raster::CreateAmbientPipelines(Device &d)
{
    // The plain scene root again with the ambient parameters, and the pipelines drawn with it: the shadow pass (DrawShadow
    // also serves the baked path) and the scene pass on the PW_SKYAMB / PW_PROBES / PW_TUBES shader.
    auto p=GraphicsParameters();
    AddAmbientParameters(p);
    graphicsRoot=GraphicsRoot(d,p,false);
    shadowPso=ShadowPso(d,graphicsRoot.Get(),Shader("raster_shadow_vs.cso"),Shader("raster_shadow_ps.cso"));
    ScenePsos(d,graphicsRoot.Get(),Shader("raster_vs.cso"),ScenePixelShader(lampShadows!=nullptr,tubes!=nullptr,probes!=nullptr,sky!=nullptr,false,false,false),scenePso,
              tubes!=nullptr);
    if(tubes) CreateTubePrepass(d,graphicsRoot.Get(),false);
}
void Raster::CreateBakedPipelines(Device &d, Baked &b)
{
    const auto vs=Shader("raster_lightmap_vs.cso"), ps=ScenePixelShader(lampShadows!=nullptr,tubes!=nullptr,probes!=nullptr,sky!=nullptr,false,false,true);
    const auto shadowVs=Shader("raster_shadow_dynamic_vs.cso"), shadowPs=Shader("raster_shadow_ps.cso");
    auto p=BakedParameters();
    AddAmbientParameters(p);
    b.root=GraphicsRoot(d,p,true);
    // Drawn with DrawShadow's root and bindings; the list variant (static triangles without lightmap UVs) with the baked root.
    b.shadowPso=b.listShadow ? ShadowPso(d,b.root.Get(),Shader("raster_shadow_list_vs.cso"),shadowPs) : ShadowPso(d,graphicsRoot.Get(),shadowVs,shadowPs);
    ScenePsos(d,b.root.Get(),vs,ps,b.scenePso,tubes!=nullptr);
    if(tubes) CreateTubePrepass(d,b.root.Get(),true);
}
void Raster::CreateCsmPipelines(Device &d, Csm &c)
{
    // One root for the cascade draws and the scene pass: the scene root's parameters (the baked root's with
    // --lightmap), then AddCsmParameters'.
    const auto shadowVs=Shader("raster_csm_shadow_vs.cso"), shadowPs=Shader("raster_shadow_ps.cso");
    const auto vs=Shader(baked ? "raster_lightmap_vs.cso" : "raster_vs.cso"), ps=ScenePixelShader(lampShadows!=nullptr,tubes!=nullptr,probes!=nullptr,sky!=nullptr,false,true,baked!=nullptr);
    auto p=baked ? BakedParameters() : GraphicsParameters();
    c.firstParameter=unsigned(p.size());
    AddCsmParameters(p);
    AddAmbientParameters(p);
    c.root=GraphicsRoot(d,p,baked!=nullptr);
    c.shadowPso=ShadowPso(d,c.root.Get(),shadowVs,shadowPs);
    if(baked) c.dynamicShadowPso=ShadowPso(d,c.root.Get(),Shader(baked->listShadow ? "raster_csm_shadow_list_vs.cso" : "raster_csm_shadow_dynamic_vs.cso"),shadowPs);
    ScenePsos(d,c.root.Get(),vs,ps,c.scenePso,tubes!=nullptr);
    if(tubes) CreateTubePrepass(d,c.root.Get(),baked!=nullptr);
}
// --lamp-tubes without --ao: the depth prepass on `root` (the scene root just built), with the scene pass's vertex shader so
// the EQUAL test meets bit-identical depths, and a depth-only pixel shader for the alpha cutoff (shaders/raster_prepass.hlsl).
// `lightmap`: the baked scene pass's shaders.
void Raster::CreateTubePrepass(Device &d, ID3D12RootSignature *root, bool lightmap)
{
    const auto vs=Shader(lightmap ? "raster_lightmap_vs.cso" : "raster_vs.cso");
    const auto ps=Shader(lightmap ? "raster_tubes_prepass_lightmap_ps.cso" : "raster_tubes_prepass_ps.cso");
    auto g=GraphicsDesc(root,vs,ps);
    g.NumRenderTargets=0; g.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_LESS;
    Check(d.gpu->CreateGraphicsPipelineState(&g,IID_PPV_ARGS(&tubes->prepassPso[0])),"Create raster tube prepass PSO");
    g.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_GREATER;
    Check(d.gpu->CreateGraphicsPipelineState(&g,IID_PPV_ARGS(&tubes->prepassPso[1])),"Create raster reverse-depth tube prepass PSO");
}
// --lamp-shadows' pass: graphicsRoot's parameters, then 6 the constants (b0 space7), 7 the triangle list (t2 space3) and 8 the
// slice drawn (b1 space7); every triangle (raster_lampshadow_vs.cso) or, with --lightmap, also Scene::unbakedTriangles alone
// (raster_lampshadow_list_vs.cso). shaders/raster_shadow.hlsl.
void Raster::CreateLampShadowPipelines(Device &d, LampShadows &l)
{
    auto p=GraphicsParameters();
    p.push_back(Cbv(0,7)); p.push_back(Srv(2,3,D3D12_SHADER_VISIBILITY_VERTEX)); p.push_back(RootConstants(1,1,D3D12_SHADER_VISIBILITY_VERTEX,7));
    l.root=GraphicsRoot(d,p,false);
    const auto ps=Shader("raster_shadow_ps.cso");
    l.allPso=ShadowPso(d,l.root.Get(),Shader("raster_lampshadow_vs.cso"),ps);
    if(l.dynamic) l.listPso=ShadowPso(d,l.root.Get(),Shader("raster_lampshadow_list_vs.cso"),ps);
}
void Raster::CreateAoPipelines(Device &d, RasterAo &a)
{
    // The scene pass's parameters of this run, then the AO table (t4 space2). The prepass draws with the scene pass's
    // own vertex shader, so the EQUAL test meets bit-identical depths; its pixel shader matches that shader's outputs.
    const auto vs=Shader(baked ? "raster_lightmap_vs.cso" : "raster_vs.cso");
    // game: the prepass's PrepassMotion variant also writes the motion (a second target, RG16F).
    const auto prepassPs=Shader(a.game ? (baked ? "raster_prepass_motion_lightmap_ps.cso" : "raster_prepass_motion_ps.cso")
                                       : baked ? "raster_prepass_lightmap_ps.cso" : "raster_prepass_ps.cso");
    const auto ps=ScenePixelShader(lampShadows!=nullptr,tubes!=nullptr,probes!=nullptr,sky!=nullptr,true,csm!=nullptr,baked!=nullptr);
    auto p=baked ? BakedParameters() : GraphicsParameters();
    if(csm) AddCsmParameters(p);
    AddAmbientParameters(p);
    a.sceneParameter=unsigned(p.size());
    p.push_back(Table(&AoTable,1,D3D12_SHADER_VISIBILITY_PIXEL));
    a.sceneRoot=GraphicsRoot(d,p,baked!=nullptr);
    auto g=GraphicsDesc(a.sceneRoot.Get(),vs,prepassPs); g.NumRenderTargets=a.game ? 2 : 1; g.RTVFormats[0]=DXGI_FORMAT_R16G16_SNORM;
    if(a.game) { g.RTVFormats[1]=DXGI_FORMAT_R16G16_FLOAT; g.BlendState.RenderTarget[1]=g.BlendState.RenderTarget[0]; }
    g.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_LESS;
    Check(d.gpu->CreateGraphicsPipelineState(&g,IID_PPV_ARGS(&a.prepassPso[0])),"Create raster prepass PSO");
    g.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_GREATER;
    Check(d.gpu->CreateGraphicsPipelineState(&g,IID_PPV_ARGS(&a.prepassPso[1])),"Create raster reverse-depth prepass PSO");
    ScenePsos(d,a.sceneRoot.Get(),vs,ps,a.scenePso,true);
    // The AO passes (shaders/raster_ao.hlsl, raster_ao_temporal.hlsl): 0 frame constants, 1 RasterAoPass (b1), 2 the
    // scene depth (t0), 3 the normal and the raw AO (t1, t2), 4 the raw AO and the AO (u0, u1); game's temporal pass:
    // 5 the AO and the motion (t3, t4), 6 the previous history (t5), 7 this frame's history (u2), s0 bilinear clamped.
    const D3D12_DESCRIPTOR_RANGE depth{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,0,0,0}, sources{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,2,1,0,0};
    const D3D12_DESCRIPTOR_RANGE targets{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,2,0,0,0};
    const D3D12_DESCRIPTOR_RANGE current{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,2,3,0,0}, history{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,5,0,0};
    const D3D12_DESCRIPTOR_RANGE historyOut{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,2,0,0};
    const D3D12_ROOT_PARAMETER compute[]={Cbv(0),RootConstants(1,6,D3D12_SHADER_VISIBILITY_ALL),Table(&depth,1,D3D12_SHADER_VISIBILITY_ALL),
        Table(&sources,1,D3D12_SHADER_VISIBILITY_ALL),Table(&targets,1,D3D12_SHADER_VISIBILITY_ALL),Table(&current,1,D3D12_SHADER_VISIBILITY_ALL),
        Table(&history,1,D3D12_SHADER_VISIBILITY_ALL),Table(&historyOut,1,D3D12_SHADER_VISIBILITY_ALL)};
    D3D12_STATIC_SAMPLER_DESC sampler=LinearWrap();
    sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP; sampler.MaxLOD=0;
    D3D12_ROOT_SIGNATURE_DESC rd{}; rd.NumParameters=UINT(std::size(compute)); rd.pParameters=compute;
    rd.NumStaticSamplers=1; rd.pStaticSamplers=&sampler;
    a.computeRoot=Root(d,rd);
    const auto occlusion=Shader(a.game ? "raster_ao_game.cso" : "raster_ao.cso"), blur=Shader(a.game ? "raster_ao_blur_game.cso" : "raster_ao_blur.cso");
    D3D12_COMPUTE_PIPELINE_STATE_DESC c{}; c.pRootSignature=a.computeRoot.Get(); c.CS={occlusion.data(),occlusion.size()};
    Check(d.gpu->CreateComputePipelineState(&c,IID_PPV_ARGS(&a.aoPso)),"Create raster AO PSO");
    c.CS={blur.data(),blur.size()};
    Check(d.gpu->CreateComputePipelineState(&c,IID_PPV_ARGS(&a.blurPso)),"Create raster AO blur PSO");
    if(!a.game) return;
    const auto temporal=Shader("raster_ao_temporal.cso");
    c.CS={temporal.data(),temporal.size()};
    Check(d.gpu->CreateComputePipelineState(&c,IID_PPV_ARGS(&a.temporalPso)),"Create raster AO temporal PSO");
}
