#include "scene.h"
#include "animation.h"
#include "gpu_skinning.h"

Scene::~Scene()
{
    animation.reset();
    if(mappedAnimation) animationUpload->Unmap(0,nullptr);
}
void Scene::Animate(float time, float previousTime, unsigned windFrame, unsigned windBefore)
{
    animationTime=time; animationMoved=dynamicTriangles>0 && (time!=previousTime || windActive);
    if(!dynamicTriangles) return;
    if(gpuSkinning) {
        animationUploadNeeded=gpuSkinning->Evaluate(animation->source,time,previousTime,windFrame,windBefore); animationCpuMs=gpuSkinning->cpuMs;
        animationRefitNeeded=gpuSkinning->geometryChanged || gpuSkinning->windChanged;
        return;
    }
    animationUploadNeeded=animation->Evaluate(time,previousTime,windFrame,windBefore); animationCpuMs=animation->cpuMs;
    animationRefitNeeded=animation->geometryChanged || animation->windChanged;
}
void Scene::RecordAnimation(Device &d) const
{
    if(!dynamicTriangles || !animationUploadNeeded) return;
    if(gpuSkinning) { gpuSkinning->Record(d,vertices.Get()); return; }
    // Device::Submit fences every frame. Reuse this mapped upload only after it retires.
    // Copy into DEFAULT memory so the many secondary rays do not read vertices over PCIe.
    Transition(d.list.Get(),vertices.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
    d.list->CopyBufferRegion(vertices.Get(),UINT64(staticTriangles)*3*sizeof(TriVertex),animationUpload.Get(),0,
        UINT64(dynamicTriangles)*3*sizeof(TriVertex));
    Transition(d.list.Get(),vertices.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
}
bool Scene::FollowPose(float time, Vec3 &position, Vec3 &heading) const
{
    if(followNode<0) return false;
    auto world=animation->source.WorldAt(followNode,time);
    position=pwgltf::ToBench(pwgltf::TransformPoint(world,{}));
    heading=pwgltf::ToBench(pwgltf::TransformDir(world,{0,0,-1})); heading.y=0;
    heading=Dot(heading,heading)>1e-10f ? Normalize(heading) : Vec3{0,0,1};
    return true;
}
