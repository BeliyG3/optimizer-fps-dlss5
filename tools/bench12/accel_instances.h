#pragma once
// The TLAS instances of the scene's structures (accel.cpp, probes.cpp; shaders/scene.hlsli TriangleIndex reads the IDs):
// InstanceID 0 the static BLAS (identity), 1 the dynamic BLAS, 2 + i SceneInstance i on its mesh's BLAS with its world rows.
#include "scene_instances.h"
#include <d3d12.h>
#include <cstring>
#include <vector>

// A zero BLAS address leaves its entries out (no static or dynamic triangles; the probes' structure passes no dynamic BLAS).
inline std::vector<D3D12_RAYTRACING_INSTANCE_DESC> TopInstances(const std::vector<InstancedMesh> &meshes,
    const std::vector<GpuInstance> &instances, D3D12_GPU_VIRTUAL_ADDRESS staticBlas, D3D12_GPU_VIRTUAL_ADDRESS dynamicBlas,
    const std::vector<D3D12_GPU_VIRTUAL_ADDRESS> &meshBlas)
{
    std::vector<D3D12_RAYTRACING_INSTANCE_DESC> result;
    auto add=[&](D3D12_GPU_VIRTUAL_ADDRESS blas, unsigned id, const float rows[3][4]) {
        if(!blas) return;
        D3D12_RAYTRACING_INSTANCE_DESC i{};
        if(rows) std::memcpy(i.Transform,rows,sizeof(i.Transform));
        else i.Transform[0][0]=i.Transform[1][1]=i.Transform[2][2]=1;
        i.InstanceID=id; i.InstanceMask=255; i.Flags=D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE;
        i.AccelerationStructure=blas; result.push_back(i);
    };
    add(staticBlas,0,nullptr); add(dynamicBlas,1,nullptr);
    for(size_t m=0;m<meshes.size() && m<meshBlas.size();++m)
        for(unsigned k=meshes[m].firstInstance;k<meshes[m].firstInstance+meshes[m].instances;++k)
            add(meshBlas[m],2+k,instances[k].world);
    return result;
}
