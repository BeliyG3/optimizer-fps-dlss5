// --render raster --ao: the depth/normal prepass and the screen-space ambient occlusion (raster_ao.h).
#include "ao_math.h"
#include "raster.h"
#include <cstring>

namespace {
constexpr auto ComputeRead=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
constexpr auto DepthReadable=D3D12_RESOURCE_STATE_DEPTH_READ|D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
constexpr auto ShaderRead=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE|D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
D3D12_RESOURCE_BARRIER Barrier(ID3D12Resource *r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER b{}; b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.pResource=r;
    b.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES; b.Transition.StateBefore=before; b.Transition.StateAfter=after;
    return b;
}
ComPtr<ID3D12Resource> Target(Device &d, unsigned w, unsigned h, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES heap{}; heap.Type=D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{}; desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D; desc.Width=w; desc.Height=h;
    desc.DepthOrArraySize=1; desc.MipLevels=1; desc.Format=format; desc.SampleDesc.Count=1; desc.Flags=flags;
    D3D12_CLEAR_VALUE clear{}; clear.Format=format; // zero, the normal target's clear
    const bool renderTarget=(flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET)!=0;
    ComPtr<ID3D12Resource> r;
    Check(d.gpu->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,state,renderTarget ? &clear : nullptr,IID_PPV_ARGS(&r)),
          "Create raster AO target");
    return r;
}
void Srv(Device &d, ID3D12Resource *r, DXGI_FORMAT format, unsigned descriptor)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC s{}; s.Format=format; s.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
    s.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; s.Texture2D.MipLevels=1;
    d.gpu->CreateShaderResourceView(r,&s,d.Cpu(descriptor));
}
void Uav(Device &d, ID3D12Resource *r, DXGI_FORMAT format, unsigned descriptor)
{
    D3D12_UNORDERED_ACCESS_VIEW_DESC u{}; u.Format=format; u.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
    d.gpu->CreateUnorderedAccessView(r,nullptr,&u,d.Cpu(descriptor));
}
// shaders/raster_ao.hlsl and raster_ao_temporal.hlsl, cbuffer RasterAoPass (b1; raster_ao.hlsl declares the first four)
struct AoPass { float radius, strength; unsigned reverse, frameIndex, historyValid; float alpha; };
}

void RasterAo::Release()
{
    normal.Reset(); raw.Reset(); out.Reset(); motion.Reset(); history[0].Reset(); history[1].Reset(); width=height=0;
}
void RasterAo::EnsureTargets(Device &d, unsigned w, unsigned h)
{
    if(normal && width==w && height==h) return;
    if(!w || !h) throw std::logic_error("RasterAo targets need the render size");
    const auto rg=DXGI_FORMAT_R16G16_SNORM, r8=DXGI_FORMAT_R8_UNORM;
    const auto uav=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    normal=Target(d,w,h,rg,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,ComputeRead);
    d.gpu->CreateRenderTargetView(normal.Get(),nullptr,NormalRtv()); Srv(d,normal.Get(),rg,srvBase);
    raw=Target(d,w,h,r8,uav,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Srv(d,raw.Get(),r8,srvBase+1); Uav(d,raw.Get(),r8,uavBase);
    out=Target(d,w,h,r8,uav,game ? ComputeRead : D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Srv(d,out.Get(),r8,srvBase+2); Uav(d,out.Get(),r8,uavBase+1);
    if(game) {
        const auto rg16f=DXGI_FORMAT_R16G16_FLOAT;
        motion=Target(d,w,h,rg16f,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,ComputeRead);
        d.gpu->CreateRenderTargetView(motion.Get(),nullptr,{NormalRtv().ptr+rtvStep}); Srv(d,motion.Get(),rg16f,srvBase+3);
        for(unsigned i=0;i<2;++i) {
            history[i]=Target(d,w,h,rg16f,uav,ShaderRead); // never read before written: the first frame's weight is 1
            Srv(d,history[i].Get(),rg16f,srvBase+4+i); Uav(d,history[i].Get(),rg16f,uavBase+2+i);
        }
    }
    width=w; height=h; recreated=true;
    std::printf("[raster] ao targets %ux%u\n",w,h);
}
void RasterAo::BeginFrame(unsigned frameIndex)
{
    historyValid=AoHistoryUsable(recreated,previousFrame,int(frameIndex));
    recreated=false; previousFrame=int(frameIndex); current^=1;
}
void RasterAo::Record(Device &d, D3D12_GPU_VIRTUAL_ADDRESS frameConstants, D3D12_GPU_DESCRIPTOR_HANDLE depth, bool reverse,
                      unsigned frameIndex)
{
    auto *list=d.list.Get();
    const auto outRest=game ? ComputeRead : D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE; // game: CSAoTemporal reads it
    Transition(list,out.Get(),outRest,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    list->SetComputeRootSignature(computeRoot.Get());
    list->SetComputeRootConstantBufferView(0,frameConstants);
    const AoPass pass{radius,strength,reverse ? 1u : 0u,frameIndex,historyValid ? 1u : 0u,alpha};
    list->SetComputeRoot32BitConstants(1,6,&pass,0);
    list->SetComputeRootDescriptorTable(2,depth);
    list->SetComputeRootDescriptorTable(3,d.Gpu(srvBase));
    list->SetComputeRootDescriptorTable(4,d.Gpu(uavBase));
    list->SetPipelineState(aoPso.Get());
    list->Dispatch((width+7)/8,(height+7)/8,1);
    Transition(list,raw.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,ComputeRead);
    list->SetPipelineState(blurPso.Get());
    list->Dispatch((width+7)/8,(height+7)/8,1);
    const D3D12_RESOURCE_BARRIER rest[]={Barrier(raw.Get(),ComputeRead,D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        Barrier(out.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,outRest)};
    list->ResourceBarrier(UINT(std::size(rest)),rest);
    if(!game) return;
    // CSAoTemporal: `out` and the motion (t3..t4), the previous history (t5) -> this frame's history (u2).
    ID3D12Resource *target=history[current].Get();
    Transition(list,target,ShaderRead,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    list->SetComputeRootDescriptorTable(5,d.Gpu(srvBase+2));
    list->SetComputeRootDescriptorTable(6,d.Gpu(srvBase+4+(current^1)));
    list->SetComputeRootDescriptorTable(7,d.Gpu(uavBase+2+current));
    list->SetPipelineState(temporalPso.Get());
    list->Dispatch((width+7)/8,(height+7)/8,1);
    Transition(list,target,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,ShaderRead);
}

void Raster::UseAo(Device &d, const Options &o)
{
    RequireCreated();
    if(o.ao!="clean" && o.ao!="game") throw std::runtime_error("--ao "+o.ao+": use --ao clean|game");
    auto next=std::make_unique<RasterAo>();
    next->radius=o.aoRadius; next->strength=o.aoStrength; next->alpha=o.aoAlpha; next->game=o.ao=="game";
    CreateAoPipelines(d,*next);
    D3D12_DESCRIPTOR_HEAP_DESC heap{}; heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV; heap.NumDescriptors=next->game ? 2 : 1;
    Check(d.gpu->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&next->rtvHeap)),"Create raster prepass RTV heap");
    next->rtvStep=d.gpu->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    next->srvBase=d.Allocate(next->game ? 6 : 3); next->uavBase=d.Allocate(next->game ? 4 : 2);
    ao=std::move(next); // only complete: Record tests `ao` alone
    std::printf("[raster] ao %s: radius %.2f m, strength %.2f, a depth/normal prepass, the scene pass tests depth EQUAL%s\n",
                o.ao.c_str(),o.aoRadius,o.aoStrength,ao->game ? "; 2 directions per frame and a temporal filter" : "");
    if(ao->game) std::printf("[raster] ao game: history blend alpha %.2f%s\n",o.aoAlpha,o.aoAlpha>=1 ? " (no history)" : "");
}
void Raster::DrawAoPrepass(Device &d, const Scene &s, D3D12_GPU_VIRTUAL_ADDRESS frameConstants, bool reverse, unsigned frameIndex)
{
    auto *list=d.list.Get();
    ao->EnsureTargets(d,width,height);
    if(ao->game) ao->BeginFrame(frameIndex);
    // The scene pass's bindings on the AO scene root, set once for the prepass and the scene pass (the AO passes
    // between them use the compute root, which leaves the graphics bindings alone).
    if(csm) BindCsm(d,s,ao->sceneRoot.Get());
    else if(baked) BindBaked(d,s,frameConstants,ao->sceneRoot.Get());
    else { list->SetGraphicsRootSignature(ao->sceneRoot.Get()); BindScene(d,s,frameConstants); }
    list->SetGraphicsRootDescriptorTable(ao->sceneParameter,d.Gpu(ao->SceneSrv()));
    const unsigned targets=ao->game ? 2 : 1; // normal, motion (game)
    ID3D12Resource *prepassTargets[]={ao->normal.Get(),ao->motion.Get()};
    for(unsigned i=0;i<targets;++i) Transition(list,prepassTargets[i],ComputeRead,D3D12_RESOURCE_STATE_RENDER_TARGET);
    ID3D12PipelineState *prepass=ao->prepassPso[reverse ? 1 : 0].Get();
    list->SetPipelineState(prepass);
    const D3D12_VIEWPORT viewport{0,0,float(width),float(height),0,1};
    const D3D12_RECT scissor{0,0,LONG(width),LONG(height)};
    list->RSSetViewports(1,&viewport); list->RSSetScissorRects(1,&scissor);
    const auto rtv=ao->NormalRtv(), dsv=dsvHeap->GetCPUDescriptorHandleForHeapStart();
    list->OMSetRenderTargets(targets,&rtv,TRUE,&dsv); // the RTVs are consecutive in ao->rtvHeap
    const float zero[4]={0,0,0,0};
    for(unsigned i=0;i<targets;++i) list->ClearRenderTargetView({rtv.ptr+i*ao->rtvStep},zero,0,nullptr);
    list->ClearDepthStencilView(dsv,D3D12_CLEAR_FLAG_DEPTH,reverse ? 0.0f : 1.0f,0,0,nullptr);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    DrawCamera(d,s,prepass);
    const D3D12_RESOURCE_BARRIER readable[]={Barrier(depthTarget.Get(),D3D12_RESOURCE_STATE_DEPTH_WRITE,DepthReadable),
        Barrier(ao->normal.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,ComputeRead),
        Barrier(ao->motion.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,ComputeRead)};
    list->ResourceBarrier(1+targets,readable); d.Mark("depth/normal prepass");
    ao->Record(d,frameConstants,d.Gpu(srvBase+2),reverse,frameIndex); // srvBase+2: the depth buffer's R32 SRV
    Transition(list,depthTarget.Get(),DepthReadable,D3D12_RESOURCE_STATE_DEPTH_WRITE); d.Mark("ao");
}
