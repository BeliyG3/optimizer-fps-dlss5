// --render raster: the Hi-Z and the cull pass of the small instances (raster_occlusion.h).
#include "raster_occlusion.h"
#include "pipeline.h"
#include <cstring>

namespace {
constexpr auto ComputeRead=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
constexpr auto DepthReadable=D3D12_RESOURCE_STATE_DEPTH_READ|D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
constexpr auto ArgsRead=D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT|D3D12_RESOURCE_STATE_COPY_SOURCE;
constexpr unsigned MaxLevels=16; // descriptors: an SRV per level, a UAV per level, the SRV of all levels
struct Pass { unsigned count, levels, width, height, reverse, runs, fromDepth, padding; }; // shaders/raster_occlusion.hlsl b1
D3D12_RESOURCE_BARRIER Barrier(ID3D12Resource *r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after, UINT subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES)
{
    D3D12_RESOURCE_BARRIER b{}; b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.pResource=r;
    b.Transition.Subresource=subresource; b.Transition.StateBefore=before; b.Transition.StateAfter=after;
    return b;
}
D3D12_ROOT_PARAMETER View(D3D12_ROOT_PARAMETER_TYPE type, unsigned reg)
{
    D3D12_ROOT_PARAMETER p{}; p.ParameterType=type; p.Descriptor={reg,0}; p.ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL; return p;
}
}

void RasterOcclusion::Create(Device &d, const Scene &s)
{
    smallInstances=(std::max<unsigned>(1,unsigned(s.instances.size()))+1)&~1u; // even: the query results stay 8-byte aligned
    maxRuns=std::max<unsigned>(1,unsigned(s.instancedMeshes.size()));
    const unsigned argsBytes=maxRuns*2*16;
    // Created inside the first raster frame's recording: upload-heap buffers only (Device::UploadBuffer would submit).
    const size_t boxBytes=std::max<size_t>(1,s.instanceBoxes.size())*sizeof(InstanceBox);
    boxes=d.Buffer(boxBytes,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
    {
        void *mappedBoxes=nullptr; D3D12_RANGE none{0,0}; Check(boxes->Map(0,&none,&mappedBoxes),"Map occlusion boxes");
        std::memset(mappedBoxes,0,boxBytes); std::memcpy(mappedBoxes,s.instanceBoxes.data(),s.instanceBoxes.size()*sizeof(InstanceBox));
        boxes->Unmap(0,nullptr);
    }
    upload=d.Buffer(UINT64(smallInstances)*sizeof(Candidate)+argsBytes,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
    void *p=nullptr; D3D12_RANGE empty{0,0}; Check(upload->Map(0,&empty,&p),"Map occlusion candidates");
    mappedCandidates=static_cast<Candidate *>(p); mappedArgs=reinterpret_cast<unsigned *>(mappedCandidates+smallInstances);
    const auto uav=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    args=d.Buffer(argsBytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COPY_DEST,uav);
    visible=d.Buffer(UINT64(smallInstances)*2*4,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,uav);
    flags=d.Buffer(UINT64(smallInstances)*4,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,uav);
    readback=d.Buffer(argsBytes+UINT64(smallInstances)*(4+8),D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
    visibleView.BufferLocation=visible->GetGPUVirtualAddress(); visibleView.SizeInBytes=smallInstances*2*4; visibleView.StrideInBytes=4;
    D3D12_INDIRECT_ARGUMENT_DESC draw{}; draw.Type=D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
    D3D12_COMMAND_SIGNATURE_DESC signature{}; signature.ByteStride=16; signature.NumArgumentDescs=1; signature.pArgumentDescs=&draw;
    Check(d.gpu->CreateCommandSignature(&signature,nullptr,IID_PPV_ARGS(&drawSignature)),"Create occlusion draw signature");
    if(check) {
        D3D12_QUERY_HEAP_DESC q{}; q.Type=D3D12_QUERY_HEAP_TYPE_OCCLUSION; q.Count=smallInstances;
        Check(d.gpu->CreateQueryHeap(&q,IID_PPV_ARGS(&queries)),"Create cull check queries");
    }
    descriptors=d.Allocate(2*MaxLevels+1);
    CreatePipelines(d);
}
void RasterOcclusion::CreatePipelines(Device &d)
{
    const D3D12_DESCRIPTOR_RANGE srv{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,0,0,0}, uavRange{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,0,0,0};
    D3D12_ROOT_PARAMETER p[9]{};
    p[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; p[0].Constants={1,0,sizeof(Pass)/4}; p[0].ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;
    p[1]=View(D3D12_ROOT_PARAMETER_TYPE_CBV,0);
    p[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; p[2].DescriptorTable={1,&srv}; p[2].ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;
    p[3].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; p[3].DescriptorTable={1,&uavRange}; p[3].ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;
    p[4]=View(D3D12_ROOT_PARAMETER_TYPE_SRV,1); p[5]=View(D3D12_ROOT_PARAMETER_TYPE_SRV,2);
    p[6]=View(D3D12_ROOT_PARAMETER_TYPE_UAV,1); p[7]=View(D3D12_ROOT_PARAMETER_TYPE_UAV,2); p[8]=View(D3D12_ROOT_PARAMETER_TYPE_UAV,3);
    D3D12_ROOT_SIGNATURE_DESC rd{}; rd.NumParameters=UINT(std::size(p)); rd.pParameters=p;
    root=Root(d,rd);
    const auto hizCs=Shader("raster_hiz.cso"), cullCs=Shader("raster_occlusion.cso");
    D3D12_COMPUTE_PIPELINE_STATE_DESC c{}; c.pRootSignature=root.Get(); c.CS={hizCs.data(),hizCs.size()};
    Check(d.gpu->CreateComputePipelineState(&c,IID_PPV_ARGS(&hizPso)),"Create Hi-Z PSO");
    c.CS={cullCs.data(),cullCs.size()};
    Check(d.gpu->CreateComputePipelineState(&c,IID_PPV_ARGS(&cullPso)),"Create occlusion cull PSO");
}
void RasterOcclusion::EnsureTargets(Device &d, unsigned w, unsigned h)
{
    if(hiz && width==w && height==h) return;
    // Level 0 covers 2x2 pixels a texel, padded to powers of two so that every level is exactly half the one below (D3D12's
    // mip sizes round down). The padding texels hold clamped copies of the edge: a texel may then be farther than its own
    // pixels, never nearer, which only culls less (hiz_math.h, unpadded, rounds the sizes up instead).
    auto pow2=[](unsigned v) { unsigned p=1; while(p<v) p*=2; return p; };
    levelWidth.clear(); levelHeight.clear();
    for(unsigned lw=pow2((w+1)/2), lh=pow2((h+1)/2);;lw=std::max(1u,lw/2), lh=std::max(1u,lh/2)) {
        levelWidth.push_back(lw); levelHeight.push_back(lh);
        if((lw==1 && lh==1) || levelWidth.size()==MaxLevels) break;
    }
    levels=unsigned(levelWidth.size());
    hiz=d.Texture(levelWidth[0],levelHeight[0],DXGI_FORMAT_R32_FLOAT,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,ComputeRead,levels);
    for(unsigned l=0;l<levels;++l) {
        D3D12_SHADER_RESOURCE_VIEW_DESC s{}; s.Format=DXGI_FORMAT_R32_FLOAT; s.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
        s.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; s.Texture2D.MostDetailedMip=l; s.Texture2D.MipLevels=1;
        d.gpu->CreateShaderResourceView(hiz.Get(),&s,d.Cpu(descriptors+l));
        D3D12_UNORDERED_ACCESS_VIEW_DESC u{}; u.Format=DXGI_FORMAT_R32_FLOAT; u.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D; u.Texture2D.MipSlice=l;
        d.gpu->CreateUnorderedAccessView(hiz.Get(),nullptr,&u,d.Cpu(descriptors+MaxLevels+l));
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC all{}; all.Format=DXGI_FORMAT_R32_FLOAT; all.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
    all.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; all.Texture2D.MipLevels=levels;
    d.gpu->CreateShaderResourceView(hiz.Get(),&all,d.Cpu(descriptors+2*MaxLevels));
    width=w; height=h;
    std::printf("[raster] occlusion: Hi-Z %ux%u, %u levels\n",levelWidth[0],levelHeight[0],levels);
}
void RasterOcclusion::Record(Device &d, const Scene &s, const RasterInstances &lists, const RasterInstances::List &camera,
                             ID3D12Resource *depth, unsigned depthSrv, D3D12_GPU_VIRTUAL_ADDRESS frameConstants, bool reverse)
{
    recorded=true; runs=unsigned(camera.smallRuns.size()); candidates=camera.smallInstances;
    if(!candidates) return;
    auto *list=d.list.Get();
    lastCandidates.clear(); lastRunInstances.clear();
    unsigned base=0;
    for(unsigned r=0;r<runs;++r) {
        const auto &run=camera.smallRuns[r]; const unsigned vertices=s.instancedMeshes[run.mesh].triangles*3;
        const unsigned visibleArgs[4]={vertices,0,0,base}, culledArgs[4]={vertices,0,0,candidates+base};
        std::memcpy(mappedArgs+r*4,visibleArgs,16); std::memcpy(mappedArgs+(runs+r)*4,culledArgs,16);
        for(unsigned k=0;k<run.count;++k) { mappedCandidates[base+k]={lists.Entries()[run.first+k],r}; lastCandidates.push_back(mappedCandidates[base+k]); }
        lastRunInstances.push_back(run.count); base+=run.count;
    }
    std::vector<D3D12_RESOURCE_BARRIER> b;
    if(argsIndirect) b={Barrier(args.Get(),ArgsRead,D3D12_RESOURCE_STATE_COPY_DEST),
        Barrier(visible.Get(),D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,D3D12_RESOURCE_STATE_UNORDERED_ACCESS)};
    if(!b.empty()) list->ResourceBarrier(UINT(b.size()),b.data());
    list->CopyBufferRegion(args.Get(),0,upload.Get(),UINT64(smallInstances)*sizeof(Candidate),UINT64(runs)*2*16);
    b={Barrier(args.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
       Barrier(depth,D3D12_RESOURCE_STATE_DEPTH_WRITE,DepthReadable)};
    list->ResourceBarrier(UINT(b.size()),b.data());
    list->SetComputeRootSignature(root.Get()); list->SetComputeRootConstantBufferView(1,frameConstants);
    list->SetPipelineState(hizPso.Get());
    for(unsigned l=0;l<levels;++l) {
        const unsigned sw=l ? levelWidth[l-1] : width, sh=l ? levelHeight[l-1] : height;
        const Pass pass{sw,sh,levelWidth[l],levelHeight[l],reverse ? 1u : 0u,0,l==0 ? 1u : 0u,0};
        auto toWrite=Barrier(hiz.Get(),ComputeRead,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,l); list->ResourceBarrier(1,&toWrite);
        list->SetComputeRoot32BitConstants(0,sizeof(Pass)/4,&pass,0);
        list->SetComputeRootDescriptorTable(2,d.Gpu(l ? descriptors+l-1 : depthSrv));
        list->SetComputeRootDescriptorTable(3,d.Gpu(descriptors+MaxLevels+l));
        list->Dispatch((levelWidth[l]+7)/8,(levelHeight[l]+7)/8,1);
        auto toRead=Barrier(hiz.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,ComputeRead,l); list->ResourceBarrier(1,&toRead);
    }
    const Pass pass{candidates,levels,width,height,reverse ? 1u : 0u,runs,0,0};
    list->SetPipelineState(cullPso.Get());
    list->SetComputeRoot32BitConstants(0,sizeof(Pass)/4,&pass,0);
    list->SetComputeRootDescriptorTable(2,d.Gpu(descriptors+2*MaxLevels));
    list->SetComputeRootDescriptorTable(3,d.Gpu(descriptors+MaxLevels));
    list->SetComputeRootShaderResourceView(4,upload->GetGPUVirtualAddress());
    list->SetComputeRootShaderResourceView(5,boxes->GetGPUVirtualAddress());
    list->SetComputeRootUnorderedAccessView(6,args->GetGPUVirtualAddress());
    list->SetComputeRootUnorderedAccessView(7,visible->GetGPUVirtualAddress());
    list->SetComputeRootUnorderedAccessView(8,flags->GetGPUVirtualAddress());
    list->Dispatch((candidates+63)/64,1,1);
    b={Barrier(args.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,ArgsRead),
       Barrier(visible.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER),
       Barrier(flags.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE),
       Barrier(depth,DepthReadable,D3D12_RESOURCE_STATE_DEPTH_WRITE)};
    list->ResourceBarrier(UINT(b.size()),b.data());
    // The counts and flags for the statistics and --cull-check, read at the next frame's BeginFrame (Submit waits the fence).
    const UINT64 argsBytes=UINT64(maxRuns)*2*16;
    list->CopyBufferRegion(readback.Get(),0,args.Get(),0,UINT64(runs)*2*16);
    list->CopyBufferRegion(readback.Get(),argsBytes,flags.Get(),0,UINT64(candidates)*4);
    auto back=Barrier(flags.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS); list->ResourceBarrier(1,&back);
    argsIndirect=true; pendingReadback=true;
}
void RasterOcclusion::DrawVisible(ID3D12GraphicsCommandList *list, const Scene &s, const RasterInstances &lists, const RasterInstances::List &camera) const
{
    if(camera.smallRuns.empty()) return;
    if(check || !recorded || !candidates) { RasterInstances::DrawSmall(list,s,camera); return; }
    list->IASetVertexBuffers(0,1,&visibleView);
    list->ExecuteIndirect(drawSignature.Get(),runs,args.Get(),0,nullptr,0);
    lists.Bind(list);
}
void RasterOcclusion::BeginFrame(const Scene &s)
{
    if(pendingReadback) {
        void *data=nullptr; const size_t argsBytes=size_t(maxRuns)*2*16;
        D3D12_RANGE range{0,argsBytes}; Check(readback->Map(0,&range,&data),"Map occlusion readback");
        const auto *words=static_cast<const unsigned *>(data);
        unsigned shown=0; for(size_t r=0;r<lastRunInstances.size();++r) shown+=words[r*4+1];
        D3D12_RANGE none{0,0}; readback->Unmap(0,&none);
        candidateTotal+=double(lastCandidates.size()); occludedTotal+=double(lastCandidates.size()-shown); ++frames;
        pendingReadback=false;
    }
    if(pendingCheck) CollectCheck(s);
    recorded=false; runs=candidates=0;
}
