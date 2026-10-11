#include "raster.h"
#include "raster_shadow.h"
#include <cstring>
#include <vector>

namespace {
constexpr auto ComputeRead=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
constexpr auto ShaderRead=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE|D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
D3D12_RESOURCE_BARRIER Barrier(ID3D12Resource *r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER b{}; b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.pResource=r;
    b.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES; b.Transition.StateBefore=before; b.Transition.StateAfter=after;
    return b;
}
void Apply(ID3D12GraphicsCommandList *list, const std::vector<D3D12_RESOURCE_BARRIER> &barriers)
{
    if(!barriers.empty()) list->ResourceBarrier(UINT(barriers.size()),barriers.data());
}
}

void Raster::RequireCreated() const
{
    if(!created) throw std::runtime_error("--render raster and --motion none need the raster shaders: "+error);
}
void Raster::WriteConstants(const Scene &s, const Options &o, const FrameConstants &c)
{
    const SunShadow sun=FitSunShadow(c.sunTravel,s.boundsMin,s.boundsMax,ShadowResolution);
    Constants k{}; k.sunVP=sun.viewProjection; sunViewProjection=sun.viewProjection; k.ambient=o.rasterAmbient; k.lightCount=lightCount;
    // One texel of depth bias and a 1.5-texel normal offset keep lit surfaces off their own shadow.
    k.shadowBias=sun.depthRange>0 ? sun.texelWorld/sun.depthRange : 0; k.shadowNormalOffset=1.5f*sun.texelWorld;
    k.skyReflections=o.SkyReflectionsOn() ? 1.0f : 0.0f; // read every frame: the menu flips it live
    // Device::Submit fences every frame, so the previous frame's constants are no longer read.
    std::memcpy(mappedConstants,&k,sizeof(k));
}
void Raster::SceneReadable(ID3D12GraphicsCommandList *list, const Scene &s, bool readable)
{
    // The scene buffers rest in the trace's compute-read state; the raster passes also read them in pixel shaders.
    const auto from=readable ? ComputeRead : ShaderRead, to=readable ? ShaderRead : ComputeRead;
    std::vector<D3D12_RESOURCE_BARRIER> b={Barrier(s.vertices.Get(),from,to),Barrier(s.materials.Get(),from,to),
        Barrier(s.groupBuffer.Get(),from,to),Barrier(s.instanceBuffer.Get(),from,to)};
    // Scene textures are only ever read: they move to both stages once and stay there (the trace's compute reads
    // are covered by that state). One scene per process (application.cpp).
    if(readable && !texturesReadable) {
        for(const auto &texture:s.textures) b.push_back(Barrier(texture.Get(),ComputeRead,ShaderRead));
        texturesReadable=true;
    }
    Apply(list,b);
}
void Raster::BindScene(Device &d, const Scene &s, D3D12_GPU_VIRTUAL_ADDRESS frameConstants)
{
    auto *list=d.list.Get(); // graphicsRoot or the baked root, which starts with the same parameters
    list->SetGraphicsRootConstantBufferView(0,frameConstants);
    list->SetGraphicsRootConstantBufferView(1,constants->GetGPUVirtualAddress());
    list->SetGraphicsRootDescriptorTable(2,d.Gpu(s.srvBase));
    list->SetGraphicsRootShaderResourceView(3,s.groupBuffer->GetGPUVirtualAddress());
    list->SetGraphicsRootShaderResourceView(4,lights->GetGPUVirtualAddress());
}
void Raster::DrawShadow(Device &d, const Scene &s, D3D12_GPU_VIRTUAL_ADDRESS frameConstants)
{
    auto *list=d.list.Get();
    list->SetGraphicsRootSignature(graphicsRoot.Get());
    BindScene(d,s,frameConstants);
    list->SetPipelineState(shadowPso.Get());
    const D3D12_VIEWPORT viewport{0,0,float(ShadowResolution),float(ShadowResolution),0,1};
    const D3D12_RECT scissor{0,0,LONG(ShadowResolution),LONG(ShadowResolution)};
    list->RSSetViewports(1,&viewport); list->RSSetScissorRects(1,&scissor);
    const D3D12_CPU_DESCRIPTOR_HANDLE dsv{dsvHeap->GetCPUDescriptorHandleForHeapStart().ptr+dsvStep};
    list->OMSetRenderTargets(0,nullptr,FALSE,&dsv);
    list->ClearDepthStencilView(dsv,D3D12_CLEAR_FLAG_DEPTH,1,0,0,nullptr);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    RasterInstances::Draw(list,s,CullShadow(s,sunViewProjection,false));
}
void Raster::DrawScene(Device &d, const Scene &s, bool reverse)
{
    // The root signature and bindings of DrawShadow stay (BindBaked's with --lightmap, BindCsm's with --csm, DrawAoPrepass's
    // with --ao).
    auto *list=d.list.Get();
    const auto &pso=ao ? ao->scenePso : csm ? csm->scenePso : baked ? baked->scenePso : scenePso;
    ID3D12PipelineState *scene=pso[reverse ? 1 : 0].Get();
    list->SetPipelineState(scene);
    list->SetGraphicsRootDescriptorTable(5,d.Gpu(srvBase));
    if(sky) list->SetGraphicsRootConstantBufferView(skyParameter,sky->constants->GetGPUVirtualAddress());
    if(probes) list->SetGraphicsRootDescriptorTable(probeParameter,d.Gpu(probes->table));
    if(tubes) {
        list->SetGraphicsRootShaderResourceView(tubeParameter,tubes->lines->GetGPUVirtualAddress());
        list->SetGraphicsRoot32BitConstant(tubeParameter+1,tubes->count,0);
    }
    if(lampShadows) BindLampShadows(d);
    BindSkyReflections(d);
    const D3D12_VIEWPORT viewport{0,0,float(width),float(height),0,1};
    const D3D12_RECT scissor{0,0,LONG(width),LONG(height)};
    list->RSSetViewports(1,&viewport); list->RSSetScissorRects(1,&scissor);
    const auto rtvStart=rtvHeap->GetCPUDescriptorHandleForHeapStart();
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv[]={rtvStart,{rtvStart.ptr+rtvStep}}, dsv=dsvHeap->GetCPUDescriptorHandleForHeapStart();
    list->OMSetRenderTargets(2,rtv,FALSE,&dsv);
    const float zero[4]={0,0,0,0};
    list->ClearRenderTargetView(rtv[0],zero,0,nullptr); list->ClearRenderTargetView(rtv[1],zero,0,nullptr);
    if(!ao && !tubes) list->ClearDepthStencilView(dsv,D3D12_CLEAR_FLAG_DEPTH,reverse ? 0.0f : 1.0f,0,0,nullptr); // --ao, --lamp-tubes: the prepass's depth
    DrawCamera(d,s,scene);
    if(hizCull) { // --cull-check's queries against the finished depth, --cull-debug's overlay
        if(!baked) hizCull->RecordCheck(d,s,cameraInstances,SceneRoot(),dsv,reverse,instanceDraws);
        hizCull->RecordDebug(d,SceneRoot(),rtv[0],instanceDraws);
    }
}
void Raster::Record(Device &d, const Scene &s, const Options &o, const FrameConstants &c,
                    D3D12_GPU_VIRTUAL_ADDRESS frameConstants, const std::array<ComPtr<ID3D12Resource>,9> &outputs,
                    unsigned uavBase)
{
    RequireCreated();
    // Review focus: never draw at a stale size. Configure resizes the raster targets with the outputs.
    const auto out=outputs[0]->GetDesc();
    if(c.width!=width || c.height!=height || out.Width!=width || out.Height!=height)
        throw std::logic_error("Raster targets do not match the frame size");
    // Projections: reverseDepth 1 (reverse) and 2 (linear-negative) are reversed, 0 and 3 standard.
    const bool reverse=c.reverse==1 || c.reverse==2;
    EnsureTargets(d,reverse); UploadLights(d,s); WriteConstants(s,o,c);
    if(csm) WriteCascades(s,o,c,frameConstants);
    if(lampShadows) WriteLampShadows(s);
    if(baked) WarnBaked(s,o);
    auto *list=d.list.Get();
    SceneReadable(list,s,true);
    if(!instanceDraws.Created()) { instanceDraws.Create(d,s); SetUpCulling(d,s,o); }
    instanceDraws.BeginFrame(list);
    frameAddress=frameConstants; frameReverse=reverse;
    if(smallCulling) {
        smallCull.Update(c.eye,c.forward);
        cullStats.smallCount+=smallCull.smallCount; cullStats.dropped+=smallCull.droppedCount; ++cullStats.frames;
    }
    if(hizCull) { hizCull->EnsureTargets(d,width,height); hizCull->BeginFrame(s); }
    // The camera's instances, with four pixels of margin for the projection jitter (currentVP is unjittered).
    cameraInstances=instanceDraws.Cull(s,c.currentVP,8.0f/float(std::min(width,height)),smallCulling ? &smallCull : nullptr,true);
    cullStats.camera+=cameraInstances.coneCulled;
    d.Mark("raster setup");
    if(lampShadows) { DrawLampShadows(d,s,frameConstants); d.Mark("lamp shadow maps"); } // first: the sun's passes below bind their own roots
    D3D12_RESOURCE_BARRIER toScene[4]; unsigned barriers=0;
    if(csm) {
        // The cascades replace the single map (and, with --lightmap, the dynamic-only map); it stays readable, unused.
        DrawCascades(d,s,csm->array.Get(),false);
        if(baked) DrawCascades(d,s,csm->dynamicArray.Get(),true);
    } else {
        Apply(list,{Barrier(shadowMap.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_DEPTH_WRITE)});
        DrawShadow(d,s,frameConstants);
        if(baked) DrawDynamicShadow(d,s,frameConstants);
        toScene[barriers++]=Barrier(shadowMap.Get(),D3D12_RESOURCE_STATE_DEPTH_WRITE,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
    toScene[barriers++]=Barrier(colourTarget.Get(),ComputeRead,D3D12_RESOURCE_STATE_RENDER_TARGET);
    toScene[barriers++]=Barrier(motionTarget.Get(),ComputeRead,D3D12_RESOURCE_STATE_RENDER_TARGET);
    toScene[barriers++]=Barrier(depthTarget.Get(),ComputeRead,D3D12_RESOURCE_STATE_DEPTH_WRITE);
    d.Mark("sun shadow");
    list->ResourceBarrier(barriers,toScene);
    if(ao) DrawAoPrepass(d,s,frameConstants,reverse,c.frame);
    else if(csm) BindCsm(d,s); else if(baked) BindBaked(d,s,frameConstants);
    if(tubes && !ao) { DrawTubePrepass(d,s,reverse); d.Mark("tube prepass"); }
    DrawScene(d,s,reverse); d.Mark("scene pass");
    Apply(list,{Barrier(colourTarget.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,ComputeRead),
        Barrier(motionTarget.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,ComputeRead),
        Barrier(depthTarget.Get(),D3D12_RESOURCE_STATE_DEPTH_WRITE,ComputeRead)});
    SceneReadable(list,s,false);
    list->SetComputeRootSignature(computeRoot.Get()); list->SetPipelineState(ResolvePso(d,o.fxaa));
    list->SetComputeRootConstantBufferView(0,frameConstants);
    list->SetComputeRootConstantBufferView(1,constants->GetGPUVirtualAddress());
    list->SetComputeRootDescriptorTable(2,d.Gpu(s.srvBase)); list->SetComputeRootDescriptorTable(3,d.Gpu(uavBase));
    list->SetComputeRootDescriptorTable(4,d.Gpu(srvBase+1)); // colour, depth, motion
    list->Dispatch((width+7)/8,(height+7)/8,1); d.Mark("resolve");
}
void Raster::ZeroMotion(Device &d, unsigned uavBase, unsigned w, unsigned h)
{
    RequireCreated();
    auto *list=d.list.Get();
    UavBarrier(list,nullptr); // after the frame's render wrote the motion guide
    list->SetComputeRootSignature(computeRoot.Get()); list->SetPipelineState(zeroPso.Get());
    list->SetComputeRootDescriptorTable(3,d.Gpu(uavBase));
    list->Dispatch((w+7)/8,(h+7)/8,1);
    UavBarrier(list,nullptr);
}
