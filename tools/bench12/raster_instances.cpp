#include "raster_instances.h"
#include <stdexcept>

void RasterInstances::Create(Device &d, const Scene &s)
{
    capacity=1+MaxLists*unsigned(s.instances.size());
    buffer=d.Buffer(UINT64(capacity)*sizeof(unsigned),D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
    void *p=nullptr; D3D12_RANGE empty{0,0}; Check(buffer->Map(0,&empty,&p),"Map raster instance lists");
    mapped=static_cast<unsigned *>(p);
    mapped[0]=0xffffffffu; // NoInstance (shaders/scene.hlsli)
    view.BufferLocation=buffer->GetGPUVirtualAddress(); view.SizeInBytes=capacity*sizeof(unsigned); view.StrideInBytes=sizeof(unsigned);
}
void RasterInstances::BeginFrame(ID3D12GraphicsCommandList *list)
{
    cursor=1;
    list->IASetVertexBuffers(0,1,&view);
}
RasterInstances::List RasterInstances::Cull(const Scene &s, const Mat4 &viewProjection, float margin, const SmallCull *culling, bool splitSmall)
{
    List result;
    for(unsigned pass=0;pass<(splitSmall ? 2u : 1u);++pass) // splitSmall: the large instances, then the small ones
        for(unsigned m=0;m<s.instancedMeshes.size();++m) {
            const auto &mesh=s.instancedMeshes[m];
            Run run{m,cursor,0};
            for(unsigned i=mesh.firstInstance;i<mesh.firstInstance+mesh.instances;++i) {
                const bool isSmall=culling && culling->smallMask[i];
                if(splitSmall && isSmall!=(pass==1)) continue;
                const auto &box=s.instanceBoxes[i];
                if(BoxOutside(viewProjection,box.lo,box.hi,margin)) continue;
                if(isSmall && culling->dropped[i]) { ++result.coneCulled; continue; }
                if(cursor>=capacity) throw std::logic_error("Raster instance lists exceed their buffer");
                mapped[cursor++]=i; ++run.count;
            }
            if(!run.count) continue;
            if(pass==1) { result.smallRuns.push_back(run); result.smallInstances+=run.count; }
            else { result.runs.push_back(run); result.instances+=run.count; }
        }
    return result;
}
void RasterInstances::DrawLarge(ID3D12GraphicsCommandList *list, const Scene &s, const List &visible)
{
    if(s.flatTriangles) list->DrawInstanced(s.flatTriangles*3,1,0,0);
    for(const auto &run:visible.runs) {
        const auto &mesh=s.instancedMeshes[run.mesh];
        list->DrawInstanced(mesh.triangles*3,run.count,0,run.first); // the vertex shader adds the mesh's first vertex
    }
}
void RasterInstances::DrawSmall(ID3D12GraphicsCommandList *list, const Scene &s, const List &visible)
{
    for(const auto &run:visible.smallRuns) list->DrawInstanced(s.instancedMeshes[run.mesh].triangles*3,run.count,0,run.first);
}
void RasterInstances::Draw(ID3D12GraphicsCommandList *list, const Scene &s, const List &visible)
{
    DrawLarge(list,s,visible); DrawSmall(list,s,visible);
}
