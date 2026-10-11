// --render raster: the culling of small instances in the raster passes (raster_cull.h, raster_occlusion.h).
#include "raster.h"

void Raster::SetUpCulling(Device &d, const Scene &s, const Options &o)
{
    if(!o.smallCulling || s.instances.empty()) return;
    smallCull.radius=o.cullSmallRadius; smallCull.coneDegrees=o.cullConeDeg; smallCull.distance=o.cullSmallDistance;
    smallCull.Classify(s.instanceBoxes);
    smallCulling=smallCull.smallCount>0;
    std::printf("[cull] %u of %zu instances are small (bounding sphere below %.2f m): culled outside a %.0f deg cone%s%s\n",
                smallCull.smallCount,s.instances.size(),double(smallCull.radius),double(smallCull.coneDegrees),
                smallCull.distance>0 ? " or beyond the distance limit" : "",o.occlusionCulling ? ", and Hi-Z occluded in the camera passes" : "");
    if(!smallCulling || !o.occlusionCulling) return;
    auto next=std::make_unique<RasterOcclusion>();
    next->check=o.cullCheck; next->debug=o.cullDebug;
    next->Create(d,s);
    hizCull=std::move(next);
    if(o.cullCheck) std::puts("[cull-check] drawing every small instance, checking the occluded ones with occlusion queries");
}
RasterInstances::List Raster::CullShadow(const Scene &s, const Mat4 &viewProjection, bool lamp)
{
    auto result=instanceDraws.Cull(s,viewProjection,ShadowCullMargin,smallCulling ? &smallCull : nullptr);
    (lamp ? cullStats.lamp : cullStats.sun)+=result.coneCulled;
    return result;
}
void Raster::DrawCamera(Device &d, const Scene &s, ID3D12PipelineState *pso)
{
    auto *list=d.list.Get();
    RasterInstances::DrawLarge(list,s,cameraInstances); // the occluders: the flattened triangles and the large instances
    if(!hizCull) { RasterInstances::DrawSmall(list,s,cameraInstances); return; }
    if(!hizCull->Recorded()) {
        // The depth buffer holds this frame's occluders: the Hi-Z and the cull pass, then this pass's pipeline again.
        hizCull->Record(d,s,instanceDraws,cameraInstances,depthTarget.Get(),srvBase+2,frameAddress,frameReverse);
        list->SetPipelineState(pso);
    }
    hizCull->DrawVisible(list,s,instanceDraws,cameraInstances);
}
ID3D12RootSignature *Raster::SceneRoot() const
{
    return ao ? ao->sceneRoot.Get() : csm ? csm->root.Get() : baked ? baked->root.Get() : graphicsRoot.Get();
}
void Raster::ReportCulling() const
{
    if(!cullStats.frames) return;
    const double f=cullStats.frames;
    std::printf("[cull] per frame: %.0f small instances, %.0f outside the cone; dropped inside a pass's view: camera %.1f, sun map or "
                "cascades %.1f, lamp spot maps %.1f (%u frames)\n",cullStats.smallCount/f,cullStats.dropped/f,cullStats.camera/f,cullStats.sun/f,
                cullStats.lamp/f,cullStats.frames);
    if(hizCull) hizCull->Report();
}
