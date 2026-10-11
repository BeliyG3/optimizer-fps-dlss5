// --render raster --lightmap: static triangles with lightmap UVs are lit by the bake (lightmap.h,
// shaders/raster_lightmap.hlsli); the triangles without baked light (dynamic ones, and static ones without lightmap UVs
// such as veg_ foliage) keep the raster lighting and cast the sun shadow that masks the baked sun on static surfaces,
// through a second shadow map that holds them alone.
#include "raster.h"

void Raster::UseLightmap(Device &d, const Scene &s, const Options &o)
{
    RequireCreated();
    // Scene threw already when --lightmap met a scene without lightmap UVs; the loader checks their atlas count.
    auto next=std::make_unique<Baked>();
    next->lightmap=LoadLightmap(d,o.lightmap,s.lightmapAtlases,lampShadows!=nullptr);
    next->listShadow=s.unbakedStaticTriangles>0;
    CreateBakedPipelines(d,*next); CreateBakedTargets(d,*next);
    baked=std::move(next); // only complete: Record tests `baked` alone
    std::printf("[raster] lightmap: static triangles take the baked light, %u dynamic triangles cast the dynamic sun shadow\n",s.dynamicTriangles);
    if(baked->listShadow) std::printf("[raster] lightmap: %u static triangles without lightmap UVs cast it too\n",s.unbakedStaticTriangles);
}
void Raster::WarnBaked(const Scene &s, const Options &o)
{
    // One warning each per run: the bake cannot follow another sun, or lamp groups the menu changed.
    if(!baked->sunWarned) {
        const auto warning=LightmapSunWarning(baked->lightmap,o);
        if(!warning.empty()) { std::printf("[warn] %s\n",warning.c_str()); baked->sunWarned=true; }
    }
    if(!baked->lampWarned && baked->lampVersion!=s.lampVersion) {
        baked->lampVersion=s.lampVersion;
        const auto warning=LightmapLampWarning(s.groups);
        if(!warning.empty()) { std::printf("[warn] %s\n",warning.c_str()); baked->lampWarned=true; }
    }
}
void Raster::DrawDynamicShadow(Device &d, const Scene &s, D3D12_GPU_VIRTUAL_ADDRESS frameConstants)
{
    // After DrawShadow: its root signature, bindings, viewport and scissor stay; the target and the triangles change. The
    // list variant needs the baked root (parameter BakedTriangleList); Record binds the scene pass's root after this.
    auto *list=d.list.Get(); auto *map=baked->shadowMap.Get();
    Transition(list,map,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_DEPTH_WRITE);
    if(baked->listShadow) {
        list->SetGraphicsRootSignature(baked->root.Get()); BindScene(d,s,frameConstants);
        list->SetGraphicsRootShaderResourceView(BakedTriangleList,s.unbakedTriangles->GetGPUVirtualAddress());
    }
    list->SetPipelineState(baked->shadowPso.Get());
    const auto dsv=baked->dsvHeap->GetCPUDescriptorHandleForHeapStart();
    list->OMSetRenderTargets(0,nullptr,FALSE,&dsv);
    list->ClearDepthStencilView(dsv,D3D12_CLEAR_FLAG_DEPTH,1,0,0,nullptr);
    // raster_shadow_dynamic_vs.cso offsets the vertex id by dynamicBase*3 (the frame constants' static triangle count);
    // raster_shadow_list_vs.cso walks Scene::unbakedTriangles.
    const unsigned triangles=baked->listShadow ? s.unbakedTriangleCount : s.dynamicTriangles;
    if(triangles) list->DrawInstanced(triangles*3,1,0,0);
    Transition(list,map,D3D12_RESOURCE_STATE_DEPTH_WRITE,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}
void Raster::BindBaked(Device &d, const Scene &s, D3D12_GPU_VIRTUAL_ADDRESS frameConstants, ID3D12RootSignature *root)
{
    // The baked root signature: DrawShadow's parameters 0..4 again (DrawScene sets 5, the full shadow map), then 6..9.
    d.list->SetGraphicsRootSignature(root ? root : baked->root.Get());
    BindScene(d,s,frameConstants);
    BindBakedParameters(d,s,baked->shadowSrv);
}
void Raster::BindBakedParameters(Device &d, const Scene &s, unsigned dynamicShadowSrv)
{
    // The baked root's parameters 6..10, also the first ones after BindScene's in the CSM root with --lightmap, where
    // the dynamic shadow is the dynamic cascade array.
    auto *list=d.list.Get();
    list->SetGraphicsRootShaderResourceView(6,s.lightmapUvs->GetGPUVirtualAddress());
    list->SetGraphicsRootDescriptorTable(7,d.Gpu(dynamicShadowSrv));
    list->SetGraphicsRootDescriptorTable(8,d.Gpu(baked->lightmap.srvBase));
    const auto &l=baked->lightmap;
    const float values[3]={l.bake.scaleFull,l.bake.scaleSun,l.rest ? 1.0f : 0.0f}; // cbuffer RasterLightmap (b2)
    list->SetGraphicsRoot32BitConstants(9,3,values,0);
    list->SetGraphicsRootShaderResourceView(BakedTriangleList,s.unbakedTriangles->GetGPUVirtualAddress());
}
