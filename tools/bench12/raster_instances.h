#pragma once
// --render raster with instanced meshes (scene_instances.h): every pass that draws the scene draws the flattened triangles as
// one range, then each instanced mesh once for its instances inside the pass's view, culled on the CPU by their world bounds.
// The visible instances' indices of every pass go into one per-frame upload buffer that the input assembler reads per
// instance (InstanceInput, raster_pipeline.cpp), so StartInstanceLocation selects a draw's run. Entry 0 is NoInstance: the
// flattened draws (StartInstanceLocation 0), including the baked mode's dynamic and list passes, read it.
// Small instances (raster_cull.h) outside the wide cone leave every pass; the camera's list keeps the small ones apart
// (smallRuns) for the Hi-Z occlusion (raster_occlusion.h).
#include "scene.h"
#include "raster_cull.h"

// True when the box lies wholly outside one side plane of the clip space of `viewProjection` (|x|, |y| <= w): it reaches no
// pixel of the pass. `margin` widens the planes (NDC units, for the camera's sub-pixel jitter). Near and far are not tested.
inline bool BoxOutside(const Mat4 &m, Vec3 lo, Vec3 hi, float margin)
{
    // Planes w*(1+margin) +- x >= 0 and +- y: the box is outside a plane when its corner farthest along the plane's normal is.
    const float scale=1+margin;
    for(int axis=0;axis<2;++axis) for(float sign:{1.0f,-1.0f}) {
        float plane[4];
        for(int k=0;k<4;++k) plane[k]=m.m[3][k]*scale+sign*m.m[axis][k];
        const float reach=plane[0]*(plane[0]>0 ? hi.x : lo.x)+plane[1]*(plane[1]>0 ? hi.y : lo.y)+plane[2]*(plane[2]>0 ? hi.z : lo.z)+plane[3];
        if(reach<0) return true;
    }
    return false;
}

class RasterInstances {
public:
    struct Run { unsigned mesh=0, first=0, count=0; }; // Scene::instancedMeshes[mesh] for buffer entries [first, first+count)
    struct List {
        std::vector<Run> runs, smallRuns; // smallRuns: only with `splitSmall` (the camera's), else small instances are in runs
        unsigned instances=0, smallInstances=0;
        unsigned coneCulled=0; // small instances inside the pass's view that the cone (or distance) dropped
    };
    // The upload buffer: room for MaxLists lists of every instance, and the NoInstance entry.
    void Create(Device &device, const Scene &scene);
    bool Created() const { return buffer!=nullptr; }
    // Each frame before the first draw: restarts the lists and binds the buffer to the input slot (the binding survives root
    // signature and pipeline changes). Device::Submit fences every frame, so the previous frame's lists are no longer read.
    void BeginFrame(ID3D12GraphicsCommandList *list);
    void Bind(ID3D12GraphicsCommandList *list) const { list->IASetVertexBuffers(0,1,&view); }
    // The instances whose bounds may reach the pass of `viewProjection`, written to the buffer. `culling` (null: no culling of
    // small instances): those it drops leave the list; splitSmall: the small ones go to smallRuns.
    List Cull(const Scene &scene, const Mat4 &viewProjection, float margin, const SmallCull *culling=nullptr, bool splitSmall=false);
    // The flattened triangles [0, Scene::flatTriangles), then one draw per run, then per small run. Root, bindings, pipeline
    // and targets are set.
    static void Draw(ID3D12GraphicsCommandList *list, const Scene &scene, const List &visible);
    // Parts of Draw: the flattened triangles and the runs (the occluders of the camera passes); the small runs.
    static void DrawLarge(ID3D12GraphicsCommandList *list, const Scene &scene, const List &visible);
    static void DrawSmall(ID3D12GraphicsCommandList *list, const Scene &scene, const List &visible);
    const unsigned *Entries() const { return mapped; } // the buffer's entries (the instance indices of the runs)
private:
    static constexpr unsigned MaxLists=64;
    ComPtr<ID3D12Resource> buffer;
    D3D12_VERTEX_BUFFER_VIEW view{};
    unsigned *mapped=nullptr;
    unsigned capacity=0, cursor=1;
};
