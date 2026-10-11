// --render raster --lamp-shadows: a spot shadow map per lamp group, as a stationary light in Unreal (raster_lamp_shadows.h
// fits them, shaders/raster_lamp_shadow.hlsli reads them). Every frame each group's slice is drawn with every triangle; with
// --lightmap a second array holds the triangles without baked light alone (characters, foliage), for the bake's lamp map.
#include "raster.h"
#include <cstdio>
#include <cstring>

namespace {
constexpr float Degrees=57.2957795f;
float Luminance(Vec3 rgb) { return Dot(rgb,{0.2126f,0.7152f,0.0722f}); }
}

void Raster::UseLampShadows(Device &d, const Scene &s, const Options &o)
{
    RequireCreated();
    auto next=std::make_unique<LampShadows>();
    next->frusta=FitLampShadows(s.lampSources,s.lampTriangles,s.groups.size(),s.boundsMin.y);
    next->groupPower.assign(s.groups.size(),Vec3{});
    for(const auto &source:s.lampSources) next->groupPower[source.group]=next->groupPower[source.group]+source.emission*source.area;
    next->dynamic=!o.lightmap.empty();
    next->count=unsigned(next->frusta.size());
    size_t groupsWithEmitters=0; for(const auto &power:next->groupPower) groupsWithEmitters+=Luminance(power)>0 ? 1 : 0;
    if(groupsWithEmitters>next->count)
        std::printf("[warn] --lamp-shadows: %zu lamp groups, only the first %u get a shadow map\n",groupsWithEmitters,next->count);
    // At least one slice, so the views are valid in a scene without lamps (the shaders then loop over none).
    const unsigned slices=std::max(1u,next->count);
    D3D12_DESCRIPTOR_HEAP_DESC heap{}; heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_DSV; heap.NumDescriptors=(next->dynamic ? 2 : 1)*slices;
    Check(d.gpu->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&next->dsvHeap)),"Create raster lamp shadow DSV heap");
    const auto dsv=next->dsvHeap->GetCPUDescriptorHandleForHeapStart();
    next->srv=d.Allocate(2); // t1, t2 space7
    next->maps=DepthArray(d,LampShadowResolution,slices,dsv,dsvStep,next->srv);
    if(next->dynamic) next->dynamicMaps=DepthArray(d,LampShadowResolution,slices,{dsv.ptr+slices*dsvStep},dsvStep,next->srv+1);
    else {
        D3D12_SHADER_RESOURCE_VIEW_DESC none{}; none.Format=DXGI_FORMAT_R32_FLOAT; none.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        none.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; none.Texture2DArray.MipLevels=1; none.Texture2DArray.ArraySize=1;
        d.gpu->CreateShaderResourceView(nullptr,&none,d.Cpu(next->srv+1)); // a null view: plain raster reads no dynamic map
    }
    next->constants=d.Buffer(2048,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
    void *p=nullptr; D3D12_RANGE empty{0,0}; Check(next->constants->Map(0,&empty,&p),"Map raster lamp shadow constants");
    next->mapped=static_cast<LampShadowConstants *>(p);
    CreateLampShadowPipelines(d,*next);
    for(const auto &f:next->frusta)
        std::printf("[raster] lamp shadow %s: apex (%.2f, %.2f, %.2f), fov %.1f deg, near %.2f far %.2f m, lit radius %.2f m\n",
                    s.groups[f.group].name.c_str(),f.apex.x,f.apex.y,f.apex.z,f.fov*Degrees,f.nearPlane,f.farPlane,f.litRadius);
    std::printf("[raster] lamp shadows: %u maps of %u^2%s\n",next->count,LampShadowResolution,
                next->dynamic ? ", and as many of the triangles without baked light" : "");
    lampShadows=std::move(next); // only complete: the Create*Pipelines functions, UploadLights, Record and DrawScene test it alone
    lightsBuilt=false; // the light slices are written with the lights
    CreateAmbientPipelines(d);
}
void Raster::WriteLampShadows(const Scene &s)
{
    // Every frame: the weights follow the menu's lamp intensity and tint. Device::Submit fences every frame, so the
    // previous frame's constants are no longer read.
    auto &l=*lampShadows;
    LampShadowConstants k{};
    for(unsigned i=0;i<l.count;++i) {
        const LampShadowFrustum &f=l.frusta[i];
        const Vec3 factor=LampFactor(s.groups[f.group]), power=l.groupPower[f.group];
        k.viewProjection[i]=f.viewProjection;
        k.apex[i][0]=f.apex.x; k.apex[i][1]=f.apex.y; k.apex[i][2]=f.apex.z; k.apex[i][3]=f.texelScale;
        k.centre[i][0]=f.centre.x; k.centre[i][1]=f.centre.y; k.centre[i][2]=f.centre.z;
        k.centre[i][3]=Luminance({power.x*factor.x,power.y*factor.y,power.z*factor.z});
    }
    k.count=l.count; k.atlasBase=baked ? unsigned(2*baked->lightmap.full.size()) : 0;
    std::memcpy(l.mapped,&k,sizeof(k));
}
void Raster::UploadLampShadowSlices(Device &d, const std::vector<unsigned> &pointGroups, const std::vector<unsigned> &lineGroups)
{
    // Called by UploadLights with the lights it built: rasterLights[i] reads entry i, rasterLines[k] entry lightCount + k.
    auto &l=*lampShadows;
    const auto groupSlice=LampShadowSlices(l.frusta,l.groupPower.size());
    std::vector<unsigned> slices;
    for(unsigned g:pointGroups) slices.push_back(g<groupSlice.size() ? groupSlice[g] : NoLampShadow);
    for(unsigned g:lineGroups) slices.push_back(g<groupSlice.size() ? groupSlice[g] : NoLampShadow);
    const unsigned needed=std::max(1u,unsigned(slices.size()));
    if(needed>l.sliceCapacity) {
        // Device::Submit fences every frame, so the previous buffer has retired and may be released here.
        l.slices=d.Buffer(UINT64(needed)*sizeof(unsigned),D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
        void *p=nullptr; D3D12_RANGE empty{0,0}; Check(l.slices->Map(0,&empty,&p),"Map raster lamp shadow slices");
        l.mappedSlices=static_cast<unsigned *>(p); l.sliceCapacity=needed;
    }
    if(!slices.empty()) std::memcpy(l.mappedSlices,slices.data(),slices.size()*sizeof(unsigned));
}
void Raster::DrawLampShadows(Device &d, const Scene &s, D3D12_GPU_VIRTUAL_ADDRESS frameConstants)
{
    auto &l=*lampShadows; auto *list=d.list.Get();
    if(!l.count) return;
    list->SetGraphicsRootSignature(l.root.Get());
    BindScene(d,s,frameConstants);
    list->SetGraphicsRootConstantBufferView(6,l.constants->GetGPUVirtualAddress());
    // The list draws read Scene::unbakedTriangles; without lightmap UVs no draw reads parameter 7, which gets a valid buffer anyway.
    list->SetGraphicsRootShaderResourceView(7,s.unbakedTriangles ? s.unbakedTriangles->GetGPUVirtualAddress() : l.slices->GetGPUVirtualAddress());
    const D3D12_VIEWPORT viewport{0,0,float(LampShadowResolution),float(LampShadowResolution),0,1};
    const D3D12_RECT scissor{0,0,LONG(LampShadowResolution),LONG(LampShadowResolution)};
    list->RSSetViewports(1,&viewport); list->RSSetScissorRects(1,&scissor);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const auto first=l.dsvHeap->GetCPUDescriptorHandleForHeapStart().ptr;
    const unsigned slices=std::max(1u,l.count);
    // Every caster into `maps`; with --lightmap the triangles without baked light alone into `dynamicMaps`.
    for(unsigned pass=0;pass<(l.dynamic ? 2u : 1u);++pass) {
        auto *array=pass ? l.dynamicMaps.Get() : l.maps.Get();
        const unsigned triangles=pass ? s.unbakedTriangleCount : 0; // pass 0: every caster, flattened and instanced
        Transition(list,array,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_DEPTH_WRITE);
        list->SetPipelineState(pass ? l.listPso.Get() : l.allPso.Get());
        for(unsigned i=0;i<l.count;++i) {
            const D3D12_CPU_DESCRIPTOR_HANDLE dsv{first+(pass*slices+i)*dsvStep};
            list->OMSetRenderTargets(0,nullptr,FALSE,&dsv);
            list->ClearDepthStencilView(dsv,D3D12_CLEAR_FLAG_DEPTH,1,0,0,nullptr);
            list->SetGraphicsRoot32BitConstant(8,i,0); // lampShadowPassSlice (b1 space7)
            if(!pass) RasterInstances::Draw(list,s,CullShadow(s,l.frusta[i].viewProjection,true));
            else if(triangles) list->DrawInstanced(triangles*3,1,0,0);
        }
        Transition(list,array,D3D12_RESOURCE_STATE_DEPTH_WRITE,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
}
void Raster::BindLampShadows(Device &d)
{
    auto &l=*lampShadows; auto *list=d.list.Get();
    list->SetGraphicsRootDescriptorTable(lampShadowParameter,d.Gpu(l.srv));
    list->SetGraphicsRootConstantBufferView(lampShadowParameter+1,l.constants->GetGPUVirtualAddress());
    list->SetGraphicsRootShaderResourceView(lampShadowParameter+2,l.slices->GetGPUVirtualAddress());
}
