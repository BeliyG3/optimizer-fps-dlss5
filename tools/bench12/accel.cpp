#include "accel.h"
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
}
Accel::Accel(Device &d,const Scene &s)
{
    auto staticGeometry=Geometry(s,0,s.staticTriangles,s.opaqueTriangles);
    dynamicGeometry=Geometry(s,s.staticTriangles,s.dynamicTriangles,s.dynamicOpaqueTriangles);
    UINT64 scratchBytes=0;
    auto allocate=[&](const auto &geometry,bool dynamic,ComPtr<ID3D12Resource> &result) {
        if(geometry.empty()) return;
        auto input=Bottom(geometry,dynamic); D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
        d.gpu->GetRaytracingAccelerationStructurePrebuildInfo(&input,&info);
        if(!info.ResultDataMaxSizeInBytes) throw std::runtime_error("Invalid BLAS prebuild size");
        result=d.Buffer(info.ResultDataMaxSizeInBytes,D3D12_HEAP_TYPE_DEFAULT,
            D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        scratchBytes=std::max(scratchBytes,std::max(info.ScratchDataSizeInBytes,info.UpdateScratchDataSizeInBytes));
    };
    allocate(staticGeometry,false,blas); allocate(dynamicGeometry,true,dynamicBlas);
    D3D12_RAYTRACING_INSTANCE_DESC desc[2]{};
    auto instance=[&](ID3D12Resource *resource,unsigned id) {
        if(!resource) return;
        auto &i=desc[instanceCount++]; i.Transform[0][0]=i.Transform[1][1]=i.Transform[2][2]=1;
        i.InstanceID=id; i.InstanceMask=255; i.Flags=D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE;
        i.AccelerationStructure=resource->GetGPUVirtualAddress();
    };
    instance(blas.Get(),0); instance(dynamicBlas.Get(),1);
    instances=d.Buffer(sizeof(desc),D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
    void *mapped=nullptr; D3D12_RANGE empty{0,0}; Check(instances->Map(0,&empty,&mapped),"Map instances");
    std::memcpy(mapped,desc,sizeof(desc)); instances->Unmap(0,nullptr);
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS top{};
    top.Type=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL; top.DescsLayout=D3D12_ELEMENTS_LAYOUT_ARRAY;
    top.NumDescs=instanceCount; top.InstanceDescs=instances->GetGPUVirtualAddress();
    top.Flags=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
    d.gpu->GetRaytracingAccelerationStructurePrebuildInfo(&top,&info);
    if(!info.ResultDataMaxSizeInBytes) throw std::runtime_error("Invalid TLAS prebuild size");
    tlas=d.Buffer(info.ResultDataMaxSizeInBytes,D3D12_HEAP_TYPE_DEFAULT,
        D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    scratch=d.Buffer(std::max(scratchBytes,info.ScratchDataSizeInBytes),D3D12_HEAP_TYPE_DEFAULT,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    d.Begin();
    auto build=[&](const auto &geometry,bool dynamic,ID3D12Resource *result) {
        if(!result) return;
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC b{}; b.Inputs=Bottom(geometry,dynamic);
        b.DestAccelerationStructureData=result->GetGPUVirtualAddress(); b.ScratchAccelerationStructureData=scratch->GetGPUVirtualAddress();
        d.list->BuildRaytracingAccelerationStructure(&b,0,nullptr); UavBarrier(d.list.Get(),result); UavBarrier(d.list.Get(),scratch.Get());
    };
    build(staticGeometry,false,blas.Get()); build(dynamicGeometry,true,dynamicBlas.Get()); BuildTop(d); d.Submit();
}
void Accel::BuildTop(Device &d)
{
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC b{};
    b.Inputs.Type=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL; b.Inputs.DescsLayout=D3D12_ELEMENTS_LAYOUT_ARRAY;
    b.Inputs.Flags=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    b.Inputs.NumDescs=instanceCount; b.Inputs.InstanceDescs=instances->GetGPUVirtualAddress();
    b.DestAccelerationStructureData=tlas->GetGPUVirtualAddress(); b.ScratchAccelerationStructureData=scratch->GetGPUVirtualAddress();
    d.list->BuildRaytracingAccelerationStructure(&b,0,nullptr); UavBarrier(d.list.Get(),tlas.Get()); UavBarrier(d.list.Get(),scratch.Get());
}
void Accel::Update(Device &d,unsigned interval,bool changed)
{
    if(!dynamicBlas || !changed) { d.Timestamp(5); d.Timestamp(6); return; }
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC b{}; b.Inputs=Bottom(dynamicGeometry,true);
    b.DestAccelerationStructureData=dynamicBlas->GetGPUVirtualAddress(); b.ScratchAccelerationStructureData=scratch->GetGPUVirtualAddress();
    if(++updates<interval) {
        b.Inputs.Flags|=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;
        b.SourceAccelerationStructureData=b.DestAccelerationStructureData;
    } else updates=0;
    d.Timestamp(5); d.list->BuildRaytracingAccelerationStructure(&b,0,nullptr);
    UavBarrier(d.list.Get(),dynamicBlas.Get()); UavBarrier(d.list.Get(),scratch.Get()); d.Timestamp(6);
    BuildTop(d);
}
