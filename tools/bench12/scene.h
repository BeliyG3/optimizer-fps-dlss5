#pragma once
#include "device.h"
#include "tri_vertex.h"
#include "options.h"
#include "lamps.h"
#include <memory>
class Animation;


struct TriMaterial {
    Vec3 albedo; float roughness; Vec3 emission; int texture; float alphaCutoff; unsigned flags;
    int metallicRoughnessTexture, normalTexture; float metallic, normalScale; unsigned lampGroup;
};
static_assert(sizeof(TriVertex)==44 && sizeof(TriMaterial)==60 && sizeof(EmissiveTriangle)==16);

class Scene {
public:
    Scene(Device &device, const Options &options);
    ~Scene();
    void Animate(float time, float previousTime);
    void CopyAnimation(Device &device) const;
    bool FollowPose(float time, Vec3 &position, Vec3 &heading) const;
    unsigned staticTriangles=0, dynamicTriangles=0, dynamicOpaqueTriangles=0;
    float animationTime=0, animationDuration=0;
    bool animationSeek=false, animationStep=false, animationMoved=false;
    bool animationUploadNeeded=false, animationRefitNeeded=false;
    double animationCpuMs=0;
    ComPtr<ID3D12Resource> vertices, materials, lights, environment;
    std::vector<ComPtr<ID3D12Resource>> textures;
    unsigned srvBase=0, textureCount=0, triangleCount=0, opaqueTriangles=0, lightCount=0;
    float lightPower=0;
    Vec3 cameraAnchor{};
    std::vector<LampGroup> groups;
    std::vector<LampSource> lampSources;
    ComPtr<ID3D12Resource> groupBuffer;
    void UpdateLamps(Device &device);
    void ApplyLamps(const std::vector<LampGroup> &settings);
private:
    std::unique_ptr<Animation> animation;
    std::vector<unsigned> dynamicOrder;
    ComPtr<ID3D12Resource> animationUpload;
    TriVertex *mappedAnimation=nullptr;
    int followNode=-1;
};
