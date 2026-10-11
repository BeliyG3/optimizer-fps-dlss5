#include "accel.h"
#include "accel_instances.h"
#include <cstdio>
#include <cstring>

namespace {
constexpr auto DynamicFlags=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE|
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS Bottom(
    const std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> &geometry, bool dynamic)
{
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS input{};
    input.Type=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL; input.DescsLayout=D3D12_ELEMENTS_LAYOUT_ARRAY;
    input.Flags=dynamic ? DynamicFlags : D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    input.NumDescs=UINT(geometry.size()); input.pGeometryDescs=geometry.data(); return input;
}
// Triangles [base, base+count) of the scene buffers, the first `opaque` of them opaque: geometry 0 those, 1 the rest.
std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> Geometry(const Scene &s,unsigned base,unsigned count,unsigned opaque)
{
    std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> result;
    auto append=[&](unsigned start,unsigned triangles,bool solid) {
        if(!triangles) return;
        D3D12_RAYTRACING_GEOMETRY_DESC g{}; g.Type=D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        g.Flags=solid ? D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE : D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
        g.Triangles.VertexBuffer.StartAddress=s.vertices->GetGPUVirtualAddress()+UINT64(start)*3*sizeof(TriVertex);
        g.Triangles.VertexBuffer.StrideInBytes=sizeof(TriVertex); g.Triangles.VertexCount=triangles*3;
        g.Triangles.VertexFormat=DXGI_FORMAT_R32G32B32_FLOAT; result.push_back(g);
    };
    append(base,opaque,true); append(base+opaque,count-opaque,false); return result;
}
D3D12_GPU_VIRTUAL_ADDRESS Address(const ComPtr<ID3D12Resource> &r) { return r ? r->GetGPUVirtualAddress() : 0; }
double Megabytes(UINT64 bytes) { return double(bytes)/1048576.0; }
}
Accel::Accel(Device &d,const Scene &s)
{
    auto staticGeometry=Geometry(s,0,s.staticTriangles,s.opaqueTriangles);
    dynamicGeometry=Geometry(s,s.staticTriangles,s.dynamicTriangles,s.dynamicOpaqueTriangles);
    std::vector<std::vector<D3D12_RAYTRACING_GEOMETRY_DESC>> meshGeometry;
    for(const auto &mesh:s.instancedMeshes) meshGeometry.push_back(Geometry(s,mesh.firstTriangle,mesh.triangles,mesh.opaqueTriangles));
    UINT64 scratchBytes=0, staticBytes=0, dynamicBytes=0, meshBytes=0;
    auto allocate=[&](const auto &geometry,bool dynamic,ComPtr<ID3D12Resource> &result,UINT64 &bytes) {
        if(geometry.empty()) return;
        auto input=Bottom(geometry,dynamic); D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
        d.gpu->GetRaytracingAccelerationStructurePrebuildInfo(&input,&info);
        if(!info.ResultDataMaxSizeInBytes) throw std::runtime_error("Invalid BLAS prebuild size");
        result=d.Buffer(info.ResultDataMaxSizeInBytes,D3D12_HEAP_TYPE_DEFAULT,
            D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        scratchBytes=std::max(scratchBytes,std::max(info.ScratchDataSizeInBytes,info.UpdateScratchDataSizeInBytes));
        bytes+=info.ResultDataMaxSizeInBytes;
    };
    allocate(staticGeometry,false,blas,staticBytes); allocate(dynamicGeometry,true,dynamicBlas,dynamicBytes);
    meshBlas.resize(meshGeometry.size());
    for(size_t m=0;m<meshGeometry.size();++m) allocate(meshGeometry[m],false,meshBlas[m],meshBytes);
    std::vector<D3D12_GPU_VIRTUAL_ADDRESS> meshAddresses;
    for(const auto &r:meshBlas) meshAddresses.push_back(Address(r));
    descs=TopInstances(s.instancedMeshes,s.instances,Address(blas),Address(dynamicBlas),meshAddresses);
    instances=d.Buffer(std::max<size_t>(1,descs.size())*sizeof(D3D12_RAYTRACING_INSTANCE_DESC),D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
    void *mapped=nullptr; D3D12_RANGE empty{0,0}; Check(instances->Map(0,&empty,&mapped),"Map instances");
    if(!descs.empty()) std::memcpy(mapped,descs.data(),descs.size()*sizeof(D3D12_RAYTRACING_INSTANCE_DESC));
    instances->Unmap(0,nullptr);
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS top{};
    top.Type=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL; top.DescsLayout=D3D12_ELEMENTS_LAYOUT_ARRAY;
    top.NumDescs=UINT(descs.size()); top.InstanceDescs=instances->GetGPUVirtualAddress();
    top.Flags=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
    d.gpu->GetRaytracingAccelerationStructurePrebuildInfo(&top,&info);
    if(!info.ResultDataMaxSizeInBytes) throw std::runtime_error("Invalid TLAS prebuild size");
    tlas=d.Buffer(info.ResultDataMaxSizeInBytes,D3D12_HEAP_TYPE_DEFAULT,
        D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    scratch=d.Buffer(std::max(scratchBytes,info.ScratchDataSizeInBytes),D3D12_HEAP_TYPE_DEFAULT,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    std::printf("[accel] BLAS: static %.1f MB, dynamic %.1f MB, %zu instanced meshes %.1f MB; TLAS %zu instances %.2f MB, scratch %.1f MB\n",
                Megabytes(staticBytes),Megabytes(dynamicBytes),meshBlas.size(),Megabytes(meshBytes),descs.size(),
                Megabytes(info.ResultDataMaxSizeInBytes),Megabytes(std::max(scratchBytes,info.ScratchDataSizeInBytes)));
    d.Begin();
    auto build=[&](const auto &geometry,bool dynamic,ID3D12Resource *result) {
        if(!result) return;
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC b{}; b.Inputs=Bottom(geometry,dynamic);
        b.DestAccelerationStructureData=result->GetGPUVirtualAddress(); b.ScratchAccelerationStructureData=scratch->GetGPUVirtualAddress();
        d.list->BuildRaytracingAccelerationStructure(&b,0,nullptr); UavBarrier(d.list.Get(),result); UavBarrier(d.list.Get(),scratch.Get());
    };
    build(staticGeometry,false,blas.Get()); build(dynamicGeometry,true,dynamicBlas.Get());
    for(size_t m=0;m<meshGeometry.size();++m) build(meshGeometry[m],false,meshBlas[m].Get());
    BuildTop(d); d.Submit();
}
std::vector<D3D12_RAYTRACING_INSTANCE_DESC> Accel::StaticInstances() const
{
    std::vector<D3D12_RAYTRACING_INSTANCE_DESC> result;
    for(const auto &desc:descs) if(desc.InstanceID!=1) result.push_back(desc);
    return result;
}
void Accel::BuildTop(Device &d)
{
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC b{};
    b.Inputs.Type=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL; b.Inputs.DescsLayout=D3D12_ELEMENTS_LAYOUT_ARRAY;
    b.Inputs.Flags=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    b.Inputs.NumDescs=UINT(descs.size()); b.Inputs.InstanceDescs=instances->GetGPUVirtualAddress();
    b.DestAccelerationStructureData=tlas->GetGPUVirtualAddress(); b.ScratchAccelerationStructureData=scratch->GetGPUVirtualAddress();
    d.list->BuildRaytracingAccelerationStructure(&b,0,nullptr); UavBarrier(d.list.Get(),tlas.Get()); UavBarrier(d.list.Get(),scratch.Get());
}
void Accel::Skip(Device &d,bool changed)
{
    stale|=changed && dynamicBlas;
    d.Timestamp(5); d.Timestamp(6);
}
void Accel::Update(Device &d,unsigned interval,bool changed)
{
    if(!dynamicBlas || !(changed || stale)) { d.Timestamp(5); d.Timestamp(6); return; }
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC b{}; b.Inputs=Bottom(dynamicGeometry,true);
    b.DestAccelerationStructureData=dynamicBlas->GetGPUVirtualAddress(); b.ScratchAccelerationStructureData=scratch->GetGPUVirtualAddress();
    // Stale (raster frames moved the geometry without refits): rebuild instead of refitting a far-off BLAS.
    if(!stale && ++updates<interval) {
        b.Inputs.Flags|=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;
        b.SourceAccelerationStructureData=b.DestAccelerationStructureData;
    } else updates=0;
    stale=false;
    d.Timestamp(5); d.list->BuildRaytracingAccelerationStructure(&b,0,nullptr);
    UavBarrier(d.list.Get(),dynamicBlas.Get()); UavBarrier(d.list.Get(),scratch.Get()); d.Timestamp(6);
    BuildTop(d);
}
