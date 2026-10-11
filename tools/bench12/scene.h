#pragma once
#include "device.h"
#include "tri_vertex.h"
#include "options.h"
#include "lamp_parts.h"
#include "lamps.h"
#include "scene_instances.h"
#include <memory>
class Animation;
class GpuSkinning;


struct TriMaterial {
    Vec3 albedo; float roughness; Vec3 emission; int texture; float alphaCutoff; unsigned flags;
    int metallicRoughnessTexture, normalTexture; float metallic, normalScale; unsigned lampGroup;
};
static_assert(sizeof(TriVertex)==44 && sizeof(TriMaterial)==60 && sizeof(EmissiveTriangle)==16);

class Scene {
public:
    Scene(Device &device, const Options &options);
    ~Scene();
    // `windFrame`: the bench frame index, the clock of --wind (independent of the animation time); `windBefore`: the wind
    // frame of the previous positions (windFrame - 1 clamped to 0, or windFrame - K for --motion-span K).
    void Animate(float time, float previousTime, unsigned windFrame=0, unsigned windBefore=0);
    // The frame's dynamic vertices into `vertices`: the CPU path's upload copied (--cpu-skinning), or the GPU skinning pass
    // (gpu_skinning.h). Either way before the dynamic BLAS refit and every read of the vertices.
    void RecordAnimation(Device &device) const;
    bool FollowPose(float time, Vec3 &position, Vec3 &heading) const;
    unsigned staticTriangles=0, dynamicTriangles=0, dynamicOpaqueTriangles=0;
    float animationTime=0, animationDuration=0;
    bool animationSeek=false, animationStep=false, animationMoved=false;
    bool animationUploadNeeded=false, animationRefitNeeded=false;
    bool windActive=false; // --wind moves veg_ nodes: every frame has new geometry (upload, BLAS refit, path-trace accumulation restart)
    double animationCpuMs=0; // the CPU part of this frame's animation: skinning (--cpu-skinning) or the poses and palettes
    bool gpuSkinned=false; // the dynamic vertices are skinned on the GPU (gpu_skinning.h; --cpu-skinning: false)
    ComPtr<ID3D12Resource> vertices, materials, lights, environment;
    ComPtr<ID3D12Resource> lightmapUvs; // LightmapUv (lightmap_uv.h) per vertex, in the order of `vertices`; only when hasLightmapUvs
    bool hasLightmapUvs=false; // --lightmap was given and some primitive has TEXCOORD_1; otherwise no lightmap buffer exists
    unsigned lightmapAtlases=0; // atlases the lightmap UVs address: 1 + their largest atlas index (0 without lightmap UVs)
    // With lightmap UVs: the indices of the triangles without baked light (UnbakedTriangles, lightmap_uv.h), at least one
    // entry; the dynamic-only shadow maps draw them. unbakedStaticTriangles: those of them that are static (veg_ foliage).
    ComPtr<ID3D12Resource> unbakedTriangles;
    unsigned unbakedTriangleCount=0, unbakedStaticTriangles=0;
    std::vector<ComPtr<ID3D12Resource>> textures;
    // The scene table at srvBase: t0 vertices, t1 materials, t2 lights, t3 environment, t4 instances, the textures from t5.
    unsigned srvBase=0, textureCount=0, triangleCount=0, opaqueTriangles=0, lightCount=0;
    // Instanced meshes (scene_instances.h): triangles [flatTriangles, triangleCount) of the scene buffers, in object space,
    // drawn and traced per instance. [0, flatTriangles) are the flattened static and dynamic triangles, drawn as one range.
    unsigned flatTriangles=0;
    std::vector<InstancedMesh> instancedMeshes;
    std::vector<GpuInstance> instances; // what instanceBuffer holds; the TLAS takes the transforms
    std::vector<InstanceBox> instanceBoxes; // world bounds per instance, for the raster passes' culling
    ComPtr<ID3D12Resource> instanceBuffer; // t4: GpuInstance per instance, one zeroed entry without instances
    float lightPower=0;
    Vec3 cameraAnchor{};
    Vec3 boundsMin{}, boundsMax{}; // all vertices at load (the raster sun shadow map is fitted to them)
    unsigned lampVersion=0; // incremented by UpdateLamps: lamp powers may have changed
    std::vector<LampGroup> groups;
    std::vector<LampSource> lampSources;
    std::vector<Vec3> lampCentroids; // parallel to lampSources
    std::vector<LampTriangle> lampTriangles; // parallel to lampSources: their corners (--lamp-tubes finds the tubes from them)
    ComPtr<ID3D12Resource> groupBuffer;
    void UpdateLamps(Device &device);
    void ApplyLamps(const std::vector<LampGroup> &settings);
private:
    std::unique_ptr<Animation> animation; // its source scene always; its CPU workers only with --cpu-skinning
    std::unique_ptr<GpuSkinning> gpuSkinning;
    std::vector<unsigned> dynamicOrder;
    ComPtr<ID3D12Resource> animationUpload;
    TriVertex *mappedAnimation=nullptr;
    int followNode=-1;
};
