#pragma once
#include "scene.h"
// The path tracer's structures: a static BLAS of the flattened static triangles, a dynamic BLAS refitted as they move, one
// BLAS per instanced mesh (scene_instances.h), and a TLAS of them all (accel_instances.h).
class Accel {
public:
    Accel(Device &device, const Scene &scene);
    void Update(Device &device, unsigned rebuildInterval, bool changed);
    // A frame that does not trace (--render raster): no structure work, the BLAS timestamps stay valid. Changed
    // geometry marks the structures stale; the next Update then rebuilds the dynamic BLAS and the TLAS in full.
    void Skip(Device &device, bool changed);
    // The TLAS instances without the dynamic BLAS: the static triangles and every instanced mesh (probes.cpp traces them).
    std::vector<D3D12_RAYTRACING_INSTANCE_DESC> StaticInstances() const;
    ComPtr<ID3D12Resource> blas, tlas;
private:
    ComPtr<ID3D12Resource> dynamicBlas, scratch, instances;
    std::vector<ComPtr<ID3D12Resource>> meshBlas; // per Scene::instancedMeshes
    std::vector<D3D12_RAYTRACING_INSTANCE_DESC> descs; // what `instances` holds
    std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> dynamicGeometry;
    unsigned updates=0;
    bool stale=false;
    void BuildTop(Device &device);
};
