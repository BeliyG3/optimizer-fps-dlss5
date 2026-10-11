// --probes: builds and traces the irradiance probe grid once at start (probes.h, shaders/probes_trace.hlsl).
#include "probes.h"
#include "accel.h"
#include "frame_constants.h"
#include "pipeline.h"
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>

namespace {
constexpr unsigned ProbeBatch=4096; // probes per dispatch: each one is submitted and waited for, far below the GPU timeout
constexpr unsigned ProbeTextures=5; // ProbeGrid::sh[0..3], then ProbeGrid::sky
struct ProbePass { // shaders/probes_trace.hlsl, cbuffer ProbePass (b1, root constants)
    Vec3 origin; float spacing; unsigned dims[3], rays, first; float invalidFraction;
};
static_assert(sizeof(ProbePass)==40);
using Clock=std::chrono::steady_clock;
double Milliseconds(Clock::time_point since) { return std::chrono::duration<double,std::milli>(Clock::now()-since).count(); }

ComPtr<ID3D12Resource> ProbeTexture(Device &d, const unsigned dims[3])
{
    D3D12_HEAP_PROPERTIES heap{}; heap.Type=D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{}; desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    desc.Width=dims[0]; desc.Height=dims[1]; desc.DepthOrArraySize=UINT16(dims[2]); desc.MipLevels=1;
    desc.Format=DXGI_FORMAT_R16G16B16A16_FLOAT; desc.SampleDesc.Count=1; desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ComPtr<ID3D12Resource> r;
    Check(d.gpu->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,nullptr,IID_PPV_ARGS(&r)),"Create probe texture");
    return r;
}
// The probes see the static geometry alone: a top-level structure over the scene's static BLAS and its instanced meshes
// (Accel builds every structure at start, raster frames never touch them), so the characters do not shadow or block the probes.
struct StaticTlas { ComPtr<ID3D12Resource> tlas, scratch, instances; };
StaticTlas BuildStaticTlas(Device &d, const Accel &accel)
{
    const auto descs=accel.StaticInstances();
    if(descs.empty()) throw std::runtime_error("--probes needs static geometry");
    StaticTlas t;
    t.instances=d.UploadBuffer(descs.data(),descs.size()*sizeof(D3D12_RAYTRACING_INSTANCE_DESC));
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC b{};
    b.Inputs.Type=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL; b.Inputs.DescsLayout=D3D12_ELEMENTS_LAYOUT_ARRAY;
    b.Inputs.Flags=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    b.Inputs.NumDescs=UINT(descs.size()); b.Inputs.InstanceDescs=t.instances->GetGPUVirtualAddress();
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
    d.gpu->GetRaytracingAccelerationStructurePrebuildInfo(&b.Inputs,&info);
    t.tlas=d.Buffer(info.ResultDataMaxSizeInBytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    t.scratch=d.Buffer(std::max<UINT64>(info.ScratchDataSizeInBytes,256),D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    b.DestAccelerationStructureData=t.tlas->GetGPUVirtualAddress(); b.ScratchAccelerationStructureData=t.scratch->GetGPUVirtualAddress();
    d.Begin(); d.list->BuildRaytracingAccelerationStructure(&b,0,nullptr); UavBarrier(d.list.Get(),t.tlas.Get()); d.Submit();
    return t;
}
// The path tracer's root (pathtrace_pipeline.cpp) with the probe outputs and constants: 0 frame constants (b0), 1 the
// structure (t0 space1), 2 the scene table, 3 lamp groups (t1 space1), 4 the five textures (u0..u4), 5 ProbePass (b1),
// 6 the scene's whole structure (t2 space1, the sky visibility's rays).
void CreateProbePipeline(Device &d, ComPtr<ID3D12RootSignature> &root, ComPtr<ID3D12PipelineState> &pso)
{
    D3D12_STATIC_SAMPLER_DESC sampler{}; sampler.Filter=D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.MaxLOD=D3D12_FLOAT32_MAX; sampler.ComparisonFunc=D3D12_COMPARISON_FUNC_ALWAYS; sampler.MaxAnisotropy=1;
    sampler.ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;
    const D3D12_DESCRIPTOR_RANGE scene[]={{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,5,0,0,0},{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,UINT_MAX,5,0,5}};
    const D3D12_DESCRIPTOR_RANGE outputs{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,ProbeTextures,0,0,0};
    D3D12_ROOT_PARAMETER p[7]{};
    p[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV; p[0].Descriptor={0,0};
    p[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV; p[1].Descriptor={0,1};
    p[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; p[2].DescriptorTable={2,scene};
    p[3].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV; p[3].Descriptor={1,1};
    p[4].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; p[4].DescriptorTable={1,&outputs};
    p[5].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; p[5].Constants={1,0,sizeof(ProbePass)/4};
    p[6].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV; p[6].Descriptor={2,1};
    D3D12_ROOT_SIGNATURE_DESC rd{}; rd.NumParameters=7; rd.pParameters=p; rd.NumStaticSamplers=1; rd.pStaticSamplers=&sampler;
    root=Root(d,rd); const auto cs=Shader("probes_trace.cso");
    D3D12_COMPUTE_PIPELINE_STATE_DESC c{}; c.pRootSignature=root.Get(); c.CS={cs.data(),cs.size()};
    Check(d.gpu->CreateComputePipelineState(&c,IID_PPV_ARGS(&pso)),"Create probe trace PSO");
}
// The path tracer's frame constants as its rays need them (Pathtrace::Render). No camera: tanHalf 0 samples the material
// textures at their full size, a huge width the sky's.
FrameConstants ProbeFrame(const Scene &s, const Options &o)
{
    FrameConstants c{}; c.albedo=o.albedo; c.sunTravel=Normalize(o.sunDir); c.sunStrength=o.sunStrength;
    c.width=1u<<20; c.height=1; c.spp=1; c.bounces=1;
    c.opaqueTriangles=s.opaqueTriangles; c.lightCount=s.lightCount; c.lightPower=s.lightPower;
    c.sunCos=std::cos(o.sunAngle*0.5f*3.14159265f/180); c.lightCandidates=unsigned(o.lightCandidates);
    c.dynamicBase=s.staticTriangles; c.dynamicOpaque=s.dynamicOpaqueTriangles;
    return c;
}
float HalfToFloat(std::uint16_t h)
{
    const unsigned sign=(h>>15)&1, exponent=(h>>10)&31, mantissa=h&1023;
    float v=exponent==0 ? std::ldexp(float(mantissa),-24) : exponent==31 ? INFINITY : std::ldexp(float(mantissa|1024),int(exponent)-25);
    return sign ? -v : v;
}
ID3D12Resource *GridTexture(const ProbeGrid &g, int k) { return k<4 ? g.sh[k].Get() : g.sky.Get(); }
// The five textures read back: 20 floats per probe (coefficients 0..3, then the sky visibility, rgba each), x fastest.
std::vector<float> ReadBack(Device &d, const ProbeGrid &g)
{
    const unsigned count=g.dims[0]*g.dims[1]*g.dims[2];
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{}; UINT64 bytes=0; const auto desc=g.sh[0]->GetDesc();
    d.gpu->GetCopyableFootprints(&desc,0,1,0,&layout,nullptr,nullptr,&bytes);
    ComPtr<ID3D12Resource> readback[ProbeTextures];
    d.Begin();
    for(int k=0;k<int(ProbeTextures);++k) {
        readback[k]=d.Buffer(bytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
        Transition(d.list.Get(),GridTexture(g,k),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION src{},dst{};
        src.pResource=GridTexture(g,k); src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.pResource=readback[k].Get(); dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint=layout;
        d.list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
        Transition(d.list.Get(),GridTexture(g,k),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
    d.Submit();
    std::vector<float> out(size_t(count)*ProbeTextures*4);
    const size_t row=layout.Footprint.RowPitch, slice=row*layout.Footprint.Height;
    for(int k=0;k<int(ProbeTextures);++k) {
        void *p=nullptr; D3D12_RANGE range{0,SIZE_T(bytes)}; Check(readback[k]->Map(0,&range,&p),"Map probe readback");
        const auto *base=static_cast<const unsigned char *>(p);
        for(unsigned z=0;z<g.dims[2];++z) for(unsigned y=0;y<g.dims[1];++y) for(unsigned x=0;x<g.dims[0];++x) {
            const auto *texel=reinterpret_cast<const std::uint16_t *>(base+z*slice+y*row+size_t(x)*8);
            const size_t probe=(size_t(z)*g.dims[1]+y)*g.dims[0]+x;
            for(int c=0;c<4;++c) out[probe*ProbeTextures*4+k*4+c]=HalfToFloat(texel[c]);
        }
        D3D12_RANGE empty{0,0}; readback[k]->Unmap(0,&empty);
    }
    return out;
}
// --probe-dump: "PWPROBE2", origin (3 floats), spacing, dims (3 uint32), rays (uint32), then ReadBack's floats (20 per probe;
// "PWPROBE1" until 2026-10-07 had the first 16 alone).
void WriteDump(const std::string &path, const ProbeGrid &g, unsigned rays, const std::vector<float> &data)
{
    std::ofstream file(path,std::ios::binary);
    if(!file) throw std::runtime_error("Cannot write --probe-dump "+path);
    const float origin[4]={g.origin.x,g.origin.y,g.origin.z,g.spacing}; const unsigned header[4]={g.dims[0],g.dims[1],g.dims[2],rays};
    file.write("PWPROBE2",8); file.write(reinterpret_cast<const char *>(origin),sizeof(origin));
    file.write(reinterpret_cast<const char *>(header),sizeof(header));
    file.write(reinterpret_cast<const char *>(data.data()),std::streamsize(data.size()*sizeof(float)));
    if(!file) throw std::runtime_error("Cannot write --probe-dump "+path);
}
}

ProbeGrid BuildProbes(Device &d, const Scene &s, const Accel &accel, const Options &o)
{
    const auto start=Clock::now();
    const ProbeLayout layout=ProbeGridLayout(s.boundsMin,s.boundsMax,o.probeSpacing);
    ProbeGrid g; g.origin=layout.origin; g.spacing=layout.spacing; std::memcpy(g.dims,layout.dims,sizeof(g.dims));
    const unsigned count=layout.Count(), uavs=d.Allocate(ProbeTextures);
    for(int k=0;k<int(ProbeTextures);++k) {
        (k<4 ? g.sh[k] : g.sky)=ProbeTexture(d,g.dims);
        D3D12_UNORDERED_ACCESS_VIEW_DESC u{}; u.Format=DXGI_FORMAT_R16G16B16A16_FLOAT; u.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE3D;
        u.Texture3D.WSize=g.dims[2]; d.gpu->CreateUnorderedAccessView(GridTexture(g,k),nullptr,&u,d.Cpu(uavs+unsigned(k)));
    }
    const StaticTlas structure=BuildStaticTlas(d,accel);
    ComPtr<ID3D12RootSignature> root; ComPtr<ID3D12PipelineState> pso; CreateProbePipeline(d,root,pso);
    const FrameConstants frame=ProbeFrame(s,o);
    const auto constants=d.Buffer(512,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
    void *mapped=nullptr; D3D12_RANGE empty{0,0}; Check(constants->Map(0,&empty,&mapped),"Map probe frame constants");
    std::memcpy(mapped,&frame,sizeof(frame)); constants->Unmap(0,nullptr);
    ProbePass pass{g.origin,g.spacing,{g.dims[0],g.dims[1],g.dims[2]},unsigned(o.probeRays),0,ProbeInvalidBackFaces};
    const auto traceStart=Clock::now(); unsigned batches=0;
    for(unsigned first=0;first<count;first+=ProbeBatch,++batches) {
        d.Begin(); ID3D12DescriptorHeap *heaps[]={d.heap.Get()}; d.list->SetDescriptorHeaps(1,heaps);
        d.list->SetComputeRootSignature(root.Get()); d.list->SetPipelineState(pso.Get());
        d.list->SetComputeRootConstantBufferView(0,constants->GetGPUVirtualAddress());
        d.list->SetComputeRootShaderResourceView(1,structure.tlas->GetGPUVirtualAddress());
        d.list->SetComputeRootDescriptorTable(2,d.Gpu(s.srvBase));
        d.list->SetComputeRootShaderResourceView(3,s.groupBuffer->GetGPUVirtualAddress());
        d.list->SetComputeRootDescriptorTable(4,d.Gpu(uavs));
        pass.first=first; d.list->SetComputeRoot32BitConstants(5,sizeof(pass)/4,&pass,0);
        d.list->SetComputeRootShaderResourceView(6,accel.tlas->GetGPUVirtualAddress());
        d.list->Dispatch(std::min(ProbeBatch,count-first),1,1);
        d.Submit();
    }
    const double traceMs=Milliseconds(traceStart);
    const auto data=ReadBack(d,g);
    unsigned invalid=0, valid=0, enclosed=0; Vec3 mean{}; double escaping=0;
    for(unsigned i=0;i<count;++i) {
        const float *p=&data[size_t(i)*ProbeTextures*4];
        if(p[3]<0.5f) { ++invalid; continue; }
        ++valid; mean=mean+Vec3{p[0],p[1],p[2]}; escaping+=p[16]; enclosed+=p[16]<0.05f ? 1 : 0;
    }
    mean=mean*(valid ? 1/(3.14159265f*float(valid)) : 0.0f);
    if(!o.probeDump.empty()) WriteDump(o.probeDump,g,unsigned(o.probeRays),data);
    std::printf("[probes] grid %ux%ux%u = %u probes, spacing %.2f m, origin (%.2f, %.2f, %.2f), %d rays each\n",
                g.dims[0],g.dims[1],g.dims[2],count,g.spacing,g.origin.x,g.origin.y,g.origin.z,o.probeRays);
    std::printf("[probes] traced in %.1f ms (%u batches), startup %.1f ms; %u invalid (%.1f %%); mean ambient of the valid ones %.3f %.3f %.3f\n",
                traceMs,batches,Milliseconds(start),invalid,count ? 100.0*invalid/count : 0.0,mean.x,mean.y,mean.z);
    std::printf("[probes] sky visibility of the valid ones: %.1f %% of the rays escape on average, %.1f %% enclosed (below 5 %%)\n",
                valid ? 100.0*escaping/valid : 0.0,valid ? 100.0*enclosed/valid : 0.0);
    return g;
}
