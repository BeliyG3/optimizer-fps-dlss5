#pragma once
#include "scene.h"
class Accel {
public:
    Accel(Device &device, const Scene &scene);
    void Update(Device &device, unsigned rebuildInterval, bool changed);
    ComPtr<ID3D12Resource> blas, tlas;
private:
    ComPtr<ID3D12Resource> dynamicBlas, scratch, instances;
    std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> dynamicGeometry;
    unsigned instanceCount=0, updates=0;
    void BuildTop(Device &device);
};
