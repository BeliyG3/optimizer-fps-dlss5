// --render raster --lamp-tubes: lamp tubes as line lights (lamp_parts.h, raster_lights.h, shaders/raster_tubes.hlsli PW_TUBES).
#include "raster.h"
#include <algorithm>
#include <cstdio>
#include <cstring>

void Raster::UseLampTubes(Device &d, const Scene &s)
{
    RequireCreated();
    auto next=std::make_unique<Tubes>();
    next->parts=FindLampParts(s.lampSources,s.lampTriangles);
    unsigned tubeCount=0; float shortest=1e30f, longest=0;
    for(const auto &part:next->parts) if(part.tube) {
        const float length=Length(part.b-part.a); ++tubeCount; shortest=std::min(shortest,length); longest=std::max(longest,length);
    }
    std::printf("[raster] lamp tubes: %zu emissive parts in %zu groups, %u tubes (%.2f..%.2f m), %zu compact\n",next->parts.size(),
                s.groups.size(),tubeCount,tubeCount ? shortest : 0.0f,longest,next->parts.size()-tubeCount);
    tubes=std::move(next); // only complete: the Create*Pipelines functions, UploadLights and DrawScene test `tubes` alone
    lightsBuilt=false; // the point lights now hold the compact emitters alone
    CreateAmbientPipelines(d);
}
std::vector<RasterLight> Raster::UploadTubes(Device &d, const Scene &s, std::vector<unsigned> &pointGroups, std::vector<unsigned> &lineGroups)
{
    const auto built=BuildRasterLampTubes(s.lampSources,s.lampCentroids,s.groups,tubes->parts);
    pointGroups=built.pointGroups; lineGroups=built.lineGroups;
    const unsigned needed=std::max(1u,unsigned(built.lines.size()));
    if(needed>tubes->capacity) {
        // Device::Submit fences every frame, so the previous buffer has retired and may be released here.
        tubes->lines=d.Buffer(UINT64(needed)*sizeof(RasterLine),D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
        void *p=nullptr; D3D12_RANGE empty{0,0}; Check(tubes->lines->Map(0,&empty,&p),"Map raster lamp tubes");
        tubes->mapped=static_cast<RasterLine *>(p); tubes->capacity=needed;
    }
    if(!built.lines.empty()) std::memcpy(tubes->mapped,built.lines.data(),built.lines.size()*sizeof(RasterLine));
    Vec3 total{}; for(const auto &line:built.lines) total=total+line.power;
    if(tubes->count!=built.lines.size() || !lightsBuilt)
        std::printf("[raster] %zu lamp tube lines, total power %.1f %.1f %.1f\n",built.lines.size(),total.x,total.y,total.z);
    tubes->count=unsigned(built.lines.size());
    return built.points;
}
void Raster::DrawTubePrepass(Device &d, const Scene &s, bool reverse)
{
    // The scene pass's root and bindings are set (DrawShadow's, BindCsm's or BindBaked's); the depth buffer is DEPTH_WRITE.
    auto *list=d.list.Get();
    ID3D12PipelineState *prepass=tubes->prepassPso[reverse ? 1 : 0].Get();
    list->SetPipelineState(prepass);
    const D3D12_VIEWPORT viewport{0,0,float(width),float(height),0,1};
    const D3D12_RECT scissor{0,0,LONG(width),LONG(height)};
    list->RSSetViewports(1,&viewport); list->RSSetScissorRects(1,&scissor);
    const auto dsv=dsvHeap->GetCPUDescriptorHandleForHeapStart();
    list->OMSetRenderTargets(0,nullptr,FALSE,&dsv);
    list->ClearDepthStencilView(dsv,D3D12_CLEAR_FLAG_DEPTH,reverse ? 0.0f : 1.0f,0,0,nullptr);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    DrawCamera(d,s,prepass);
}
