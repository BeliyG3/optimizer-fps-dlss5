#pragma once
#include "frame_constants.h"
#include "lightmap.h"
#include "probes.h"
#include "raster_ao.h"
#include "raster_instances.h"
#include "raster_lamp_shadows.h"
#include "raster_lights.h"
#include "raster_occlusion.h"
#include "scene.h"
#include <array>
#include <memory>
#include <string>
#include <vector>

// --render raster: the lab drawn like a game, without ray tracing. A depth-only sun shadow map, a scene pass with
// the path tracer's materials lit by the shadowed sun, the lamp groups as point lights, emission and an ambient
// term, then a compute resolve that adds sky and fog and writes the same outputs the path tracer fills
// (colour, depth in the --depth convention, motion; the RR guides cleared). Everything after it is shared.
// --lightmap adds baked light on static geometry (UseLightmap).
class Raster {
public:
    // Root signatures, PSOs, descriptor heaps. Missing raster shaders are remembered, not thrown: only a raster
    // frame or --motion none needs them.
    void Create(Device &device);
    // The render size changed: the targets are recreated at the next raster frame.
    void Resize(Device &device, unsigned width, unsigned height);
    // Records the raster frame and writes outputs[0] colour, [1] depth (the --depth convention), [2] motion,
    // clears [3..6]. Leaves outputs in UNORDERED_ACCESS, as the trace dispatch does.
    void Record(Device &device, const Scene &scene, const Options &options, const FrameConstants &c,
                D3D12_GPU_VIRTUAL_ADDRESS frameConstants, const std::array<ComPtr<ID3D12Resource>,9> &outputs,
                unsigned uavBase);
    // --motion none, either render mode: zeroes outputs[2] (UAV) after the frame's render.
    void ZeroMotion(Device &device, unsigned uavBase, unsigned width, unsigned height);
    // --lightmap, once after Create (raster_lightmap.cpp): loads the atlases and creates what only the baked mode uses.
    // Static triangles with lightmap UVs then take the baked light, their sun part shadowed by a second shadow map
    // that holds the dynamic triangles alone; everything else is shaded as before. Without it the frame is unchanged.
    void UseLightmap(Device &device, const Scene &scene, const Options &options);
    // --csm, once after UseLightmap (raster_csm.cpp): the sun shadow in four cascades of ShadowResolution^2 fitted to the
    // view (raster_csm.h) instead of the single map; with --lightmap the dynamic-only map becomes cascades too.
    void UseCsm(Device &device, const Options &options);
    // --ao, once after UseCsm (raster_ao.cpp): a depth/normal prepass and screen-space ambient occlusion before the
    // scene pass, which then tests depth EQUAL and occludes its ambient and lamp diffuse light (raster_ao.h).
    void UseAo(Device &device, const Options &options);
    // --raster-sky-ambient, once after Create and before UseLightmap (raster_sky.cpp): the shading's ambient term is the --hdri sky's
    // SH9 irradiance by the surface normal (sky_sh.h) instead of the constant --raster-ambient, in RasterShade only (so with --lightmap
    // the baked static pixels keep their baked light). Rebuilds the scene pass on the PW_SKYAMB shaders; the later Use* functions
    // pick the same variants. Without it nothing changes.
    void UseSkyAmbient(Device &device, const Options &options);
    // --probes, once after UseSkyAmbient and before UseLightmap (raster_probes.cpp): the ambient term of RasterShade comes from the
    // traced probe grid (BuildProbes) where it has valid probes, else from the sky or the constant as before. Rebuilds the scene
    // pass on the PW_PROBES shaders; the later Use* functions pick the same variants. Without it nothing changes.
    void UseProbes(Device &device, const ProbeGrid &grid);
    // --lamp-tubes, once after UseProbes and before UseLightmap (raster_tubes.cpp): every emissive tube of a lamp group becomes a
    // line light (lamp_parts.h, raster_lights.h); compact emitters stay point lights. Rebuilds the scene pass on the PW_TUBES shaders;
    // the later Use* functions pick the same variants. Without it nothing changes.
    void UseLampTubes(Device &device, const Scene &scene);
    // --lamp-shadows, once after UseLampTubes and before UseLightmap (raster_lamp_shadow_passes.cpp): a spot shadow map per lamp
    // group (raster_lamp_shadows.h) drawn every frame, which shadows the group's point light and tubes. With --lightmap a second
    // set holds the triangles without baked light alone, for the bake's lamp map (LoadLightmap then needs "lamp" maps).
    // Rebuilds the scene pass on the PW_LAMPSHADOW shaders; the later Use* functions pick the same variants.
    void UseLampShadows(Device &device, const Scene &scene, const Options &options);
    // --sky-reflections (raster_reflections.cpp, RASTER_REFLECTIONS.md), once after Create and before the first raster frame, in every
    // run: the sky's prefiltered cube (built on the GPU from the scene's panorama, Sky()'s mapping) and the split sum's DFG table.
    // Every scene pixel shader reads them, so every scene root has their table; the scene pass adds the sky's specular while
    // Options::skyReflections is on (the menu flips it live). No raster shaders (a path-traced run): nothing.
    void UseSkyReflections(Device &device, const Scene &scene);
    // --fxaa (raster_fxaa.cpp): with Options::fxaa the resolve runs FXAA 3.11 on the finished colour (fog and sky in) before writing it, so
    // the upscaler, NR and the add-on receive the filtered frame. Record picks the resolve pipeline by the option every frame (the menu
    // flips it live); the FXAA pipeline is created on first use, or ahead of the first frame by UseFxaa. Without it nothing changes.
    void UseFxaa(Device &device);
    // The culling of small instances over the run (raster_cull.h, raster_occlusion.h): cone-culled per pass, occluded.
    void ReportCulling() const;
private:
    static constexpr unsigned ShadowResolution=2048;
    struct Constants { // shaders/raster_common.hlsli, cbuffer RasterFrame (b1)
        Mat4 sunVP;
        float ambient; unsigned lightCount; float shadowBias, shadowNormalOffset;
        float skyReflections, padding[3]; // 1 while Options::skyReflections is on
    };
    static_assert(sizeof(Constants)==96);
    void RequireCreated() const;
    // raster_resources.cpp
    void CreateDescriptors(Device &device);
    void EnsureTargets(Device &device, bool reverse);
    void UploadLights(Device &device, const Scene &scene);
    // --lamp-tubes: uploads the lines, returns the points; the groups of both for --lamp-shadows.
    std::vector<RasterLight> UploadTubes(Device &device, const Scene &scene, std::vector<unsigned> &pointGroups, std::vector<unsigned> &lineGroups);
    // --lamp-tubes without --ao: a depth prepass on the scene pass's root (raster_pipeline.cpp), drawn just before DrawScene, whose
    // pipelines then test EQUAL (raster_tubes.cpp).
    void CreateTubePrepass(Device &device, ID3D12RootSignature *root, bool lightmap);
    void DrawTubePrepass(Device &device, const Scene &scene, bool reverse);
    // raster.cpp
    void WriteConstants(const Scene &scene, const Options &options, const FrameConstants &c);
    void SceneReadable(ID3D12GraphicsCommandList *list, const Scene &scene, bool readable);
    void BindScene(Device &device, const Scene &scene, D3D12_GPU_VIRTUAL_ADDRESS frameConstants); // root parameters 0..4
    void DrawShadow(Device &device, const Scene &scene, D3D12_GPU_VIRTUAL_ADDRESS frameConstants);
    void DrawScene(Device &device, const Scene &scene, bool reverse);
    // --lightmap: raster_pipeline.cpp, raster_resources.cpp, raster_lightmap.cpp
    struct Baked;
    void CreateBakedPipelines(Device &device, Baked &baked);
    void CreateBakedTargets(Device &device, Baked &baked);
    void WarnBaked(const Scene &scene, const Options &options);
    void DrawDynamicShadow(Device &device, const Scene &scene, D3D12_GPU_VIRTUAL_ADDRESS frameConstants);
    // root: the baked root, or --ao's scene root (which starts with its parameters).
    void BindBaked(Device &device, const Scene &scene, D3D12_GPU_VIRTUAL_ADDRESS frameConstants, ID3D12RootSignature *root=nullptr);
    void BindBakedParameters(Device &device, const Scene &scene, unsigned dynamicShadowSrv); // root parameters 6..10
    // The baked root's parameter 10 (raster_pipeline.cpp BakedParameters): Scene::unbakedTriangles for the list shadow draws.
    static constexpr unsigned BakedTriangleList=10;
    // --csm: raster_csm.cpp, raster_pipeline.cpp
    struct Csm;
    void CreateCsmPipelines(Device &device, Csm &csm);
    void WriteCascades(const Scene &scene, const Options &options, const FrameConstants &c, D3D12_GPU_VIRTUAL_ADDRESS frameConstants);
    // Clears and draws the four slices of `array` (every triangle, or the dynamic ones alone), leaving it readable.
    void DrawCascades(Device &device, const Scene &scene, ID3D12Resource *array, bool dynamicOnly);
    void BindCsm(Device &device, const Scene &scene, ID3D12RootSignature *root=nullptr); // root: as BindBaked's
    // --raster-sky-ambient, --probes and --lamp-tubes: raster_sky.cpp, raster_probes.cpp, raster_tubes.cpp, raster_pipeline.cpp.
    // CreateAmbientPipelines rebuilds the plain scene root and its pipelines; AddAmbientParameters appends the sky's CBV, the probes'
    // table and the tubes' lines and count to any scene root.
    void CreateAmbientPipelines(Device &device);
    void AddAmbientParameters(std::vector<D3D12_ROOT_PARAMETER> &parameters);
    // --sky-reflections: raster_reflections.cpp, raster_pipeline.cpp (the build's compute root and pipelines, used once).
    struct SkyPrefilterPipelines { ComPtr<ID3D12RootSignature> root; ComPtr<ID3D12PipelineState> source, downsample, prefilter; };
    static SkyPrefilterPipelines CreateSkyPrefilterPipelines(Device &device);
    void BindSkyReflections(Device &device); // the scene root's reflection table (DrawScene)
    // --ao: raster_ao.cpp, raster_pipeline.cpp
    void CreateAoPipelines(Device &device, RasterAo &ao);
    // --lamp-shadows: raster_lamp_shadow_passes.cpp, raster_pipeline.cpp
    struct LampShadows;
    void CreateLampShadowPipelines(Device &device, LampShadows &lamp);
    void WriteLampShadows(const Scene &scene);
    void UploadLampShadowSlices(Device &device, const std::vector<unsigned> &pointGroups, const std::vector<unsigned> &lineGroups);
    void DrawLampShadows(Device &device, const Scene &scene, D3D12_GPU_VIRTUAL_ADDRESS frameConstants);
    void BindLampShadows(Device &device); // the scene root's lamp shadow parameters (DrawScene)
    // A typeless R32 array of `slices` size^2 depth maps: a D32 view per slice (firstDsv on), one R32_FLOAT array SRV at
    // `srv`; it rests in PIXEL_SHADER_RESOURCE (raster_csm.cpp). The cascades and the lamp shadow maps.
    static ComPtr<ID3D12Resource> DepthArray(Device &device, unsigned size, unsigned slices, D3D12_CPU_DESCRIPTOR_HANDLE firstDsv,
                                             unsigned dsvStep, unsigned srv);
    // Binds the scene pass on --ao's scene root, draws the prepass into the depth buffer (DEPTH_WRITE on entry and on
    // return) and the normal, and records the occlusion.
    void DrawAoPrepass(Device &device, const Scene &scene, D3D12_GPU_VIRTUAL_ADDRESS frameConstants, bool reverse, unsigned frameIndex);
    // Instanced meshes (raster_instances.h): the per-frame lists, the camera's (the AO and tube prepasses and the scene pass draw
    // it), the sun map's view (WriteConstants). Shadow passes cull with ShadowCullMargin.
    static constexpr float ShadowCullMargin=1e-3f;
    RasterInstances instanceDraws;
    RasterInstances::List cameraInstances;
    // Small instances (raster_cull.h, raster_occlusion.h; Options::smallCulling, occlusionCulling): set up by the first frame.
    SmallCull smallCull;
    bool smallCulling=false;
    std::unique_ptr<RasterOcclusion> hizCull;
    D3D12_GPU_VIRTUAL_ADDRESS frameAddress=0; // this frame's constants, for the occlusion pass inside the first camera pass
    bool frameReverse=false;
    struct CullStats { double camera=0, sun=0, lamp=0, smallCount=0, dropped=0; unsigned frames=0; } cullStats; // cone-culled instances
    void SetUpCulling(Device &device, const Scene &scene, const Options &options);
    // A shadow pass's instances (the sun map, a cascade: lamp false; a lamp spot map: true): its frustum and the cone.
    RasterInstances::List CullShadow(const Scene &scene, const Mat4 &viewProjection, bool lamp);
    // A camera pass's instances with `pso` set: the occluders, the occlusion pass on the first one, the visible small ones.
    void DrawCamera(Device &device, const Scene &scene, ID3D12PipelineState *pso);
    ID3D12RootSignature *SceneRoot() const; // the scene pass's root signature of this run
    Mat4 sunViewProjection{};
    std::string error;
    bool created=false, texturesReadable=false, targetsReverse=false;
    ComPtr<ID3D12RootSignature> graphicsRoot, computeRoot;
    ComPtr<ID3D12PipelineState> shadowPso, scenePso[2], resolvePso, zeroPso; // scenePso: [0] LESS, [1] GREATER (reverse)
    ComPtr<ID3D12PipelineState> fxaaResolvePso; // raster_fxaa.cpp, created lazily
    ID3D12PipelineState *ResolvePso(Device &device, bool fxaa);
    ComPtr<ID3D12DescriptorHeap> rtvHeap, dsvHeap;
    ComPtr<ID3D12Resource> colourTarget, motionTarget, depthTarget, shadowMap, constants, lights;
    Constants *mappedConstants=nullptr;
    RasterLight *mappedLights=nullptr;
    unsigned lightCapacity=0, lightCount=0, lampVersion=0;
    bool lightsBuilt=false;
    unsigned srvBase=0, width=0, height=0, rtvStep=0, dsvStep=0;
    // Everything only --lightmap creates; null without it.
    struct Baked {
        LightmapSet lightmap;
        ComPtr<ID3D12RootSignature> root; // graphicsRoot's parameters, then the lightmap's (raster_pipeline.cpp)
        ComPtr<ID3D12PipelineState> shadowPso, scenePso[2]; // the dynamic-only shadow pass; the scene pass [0] LESS, [1] GREATER
        ComPtr<ID3D12DescriptorHeap> dsvHeap;
        ComPtr<ID3D12Resource> shadowMap; // the sun shadow map of the triangles without baked light alone, ShadowResolution^2
        unsigned shadowSrv=0, lampVersion=0;
        bool sunWarned=false, lampWarned=false;
        // Static triangles without lightmap UVs (veg_ foliage): the dynamic-only maps walk Scene::unbakedTriangles
        // (raster_shadow_list_vs.cso, on the baked root) instead of the dynamic range (on DrawShadow's root).
        bool listShadow=false;
    };
    std::unique_ptr<Baked> baked;
    struct CsmConstants { // shaders/raster_csm.hlsli, cbuffer RasterCascades (b2 space1)
        Mat4 cascadeVP[4];
        float splitFar[4], texelWorld[4], bias[4];
        Vec3 forward; float blend;
    };
    static_assert(sizeof(CsmConstants)==320);
    // Everything only --csm creates; null without it.
    struct Csm {
        // graphicsRoot's parameters (the baked root's with --lightmap), then the cascades CBV at firstParameter, the
        // cascade array (t3 space2) and the shadow pass's cascade index (b3 space1) (raster_pipeline.cpp).
        ComPtr<ID3D12RootSignature> root;
        ComPtr<ID3D12PipelineState> shadowPso, dynamicShadowPso, scenePso[2]; // dynamicShadowPso: --lightmap only
        ComPtr<ID3D12DescriptorHeap> dsvHeap; // a DSV per slice: `array`, then `dynamicArray`
        ComPtr<ID3D12Resource> array, dynamicArray, constants; // the arrays: 4 slices of ShadowResolution^2, R32
        CsmConstants *mapped=nullptr;
        Mat4 viewProjection[4]{}; // this frame's cascades, for the instance culling (DrawCascades)
        unsigned srv=0, dynamicSrv=0, firstParameter=0;
        D3D12_GPU_VIRTUAL_ADDRESS frameConstants=0; // this frame's, for DrawCascades and BindCsm
    };
    std::unique_ptr<Csm> csm;
    std::unique_ptr<RasterAo> ao; // null without --ao
    // Everything only --raster-sky-ambient creates; null without it.
    struct Sky {
        ComPtr<ID3D12Resource> constants; // cbuffer RasterSky (b4 space1): float4 sh[9], written once
    };
    std::unique_ptr<Sky> sky;
    // Everything only --probes creates; null without it.
    struct Probes {
        ProbeGrid grid;
        ComPtr<ID3D12Resource> constants; // cbuffer RasterProbes (b0 space5), written once
        unsigned table=0; // the CBV, then the four textures' SRVs (t0..t3 space5)
    };
    std::unique_ptr<Probes> probes;
    // Everything only --lamp-tubes creates; null without it.
    struct Tubes {
        std::vector<LampPart> parts; // the lamp emitters' connected parts, found once (the lamp geometry is static)
        ComPtr<ID3D12Resource> lines; // RasterLine[capacity], upload heap (t0 space6)
        RasterLine *mapped=nullptr;
        unsigned capacity=0, count=0; // count: the root constant rasterLineCount (b0 space6)
        ComPtr<ID3D12PipelineState> prepassPso[2]; // without --ao: depth only, [0] LESS, [1] GREATER, on the last scene root built
    };
    std::unique_ptr<Tubes> tubes;
    struct LampShadowConstants { // shaders/raster_lamp_shadow.hlsli, cbuffer RasterLampShadows (b0 space7)
        Mat4 viewProjection[MaxLampShadows];
        float apex[MaxLampShadows][4], centre[MaxLampShadows][4]; // xyz, then texelScale / the group's luminance power
        unsigned count, atlasBase; float padding[2];
    };
    static_assert(sizeof(LampShadowConstants)==MaxLampShadows*96+16);
    // Everything only --lamp-shadows creates; null without it.
    struct LampShadows {
        std::vector<LampShadowFrustum> frusta; // slice i: frusta[i]
        std::vector<Vec3> groupPower; // per group: sum(area * emission), before the menu's LampFactor
        bool dynamic=false; // --lightmap: the second array, of the triangles without baked light
        // The pass's root: graphicsRoot's parameters, then 6 the constants (b0 space7), 7 the triangle list (t2 space3, VS),
        // 8 the slice drawn (b1 space7). allPso: every triangle; listPso: Scene::unbakedTriangles (--lightmap).
        ComPtr<ID3D12RootSignature> root;
        ComPtr<ID3D12PipelineState> allPso, listPso;
        ComPtr<ID3D12DescriptorHeap> dsvHeap; // a DSV per slice: `maps`, then `dynamicMaps`
        ComPtr<ID3D12Resource> maps, dynamicMaps, constants, slices; // slices: uint per raster light then per line (t3 space7)
        LampShadowConstants *mapped=nullptr;
        unsigned *mappedSlices=nullptr;
        unsigned srv=0, sliceCapacity=0, count=0; // srv: the maps' SRV, then the dynamic maps' (or a null view); count: slices
    };
    std::unique_ptr<LampShadows> lampShadows;
    // --sky-reflections' resources (UseSkyReflections); every raster frame needs them.
    struct Reflections {
        ComPtr<ID3D12Resource> source, cube, dfg; // source: the build's (released after it); cube: prefiltered; dfg: (A, B) R32G32
        unsigned table=0; // the cube's and the DFG table's SRVs (t0, t1 space8)
    };
    std::unique_ptr<Reflections> reflections;
    // The root parameters of the sky's CBV and the probes' table in the scene pass's root signature: each root built after the
    // plain one (baked, CSM, AO) appends them after its other parameters (AddAmbientParameters) and records their indices here,
    // so the last one built is the one DrawScene binds.
    unsigned skyParameter=0, probeParameter=0, tubeParameter=0; // tubeParameter: the lines' SRV, then the count at +1
    unsigned lampShadowParameter=0; // --lamp-shadows: the maps' table, then the constants at +1 and the light slices at +2
    unsigned reflectionParameter=0; // --sky-reflections' table, in every scene root (AddAmbientParameters, after the others)
};
