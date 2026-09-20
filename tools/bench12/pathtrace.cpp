#include "pathtrace.h"
#include <cstring>

Pathtrace::Pathtrace(Device &d)
{
    CreatePipelines(d);
    constants=d.Buffer(512,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
    uavBase=d.Allocate(10); srvBase=d.Allocate(12);
    pickBuffer=d.Buffer(8,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    pickReadback=d.Buffer(8,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_UNORDERED_ACCESS_VIEW_DESC u{}; u.ViewDimension=D3D12_UAV_DIMENSION_BUFFER;
    u.Buffer.NumElements=1; u.Buffer.StructureByteStride=8;
    d.gpu->CreateUnorderedAccessView(pickBuffer.Get(),nullptr,&u,d.Cpu(uavBase+8));
}
void Pathtrace::Render(Device &d, const Scene &s, Accel &a, Options &o, int frame, bool dump, const CameraState *pose, void (*overlay)(Device &),float frameDelta)
{
    Configure(d,o);
    CameraState camera=pose ? *pose : CameraAt(frame,o,s.cameraAnchor);
    if(!initialized || CameraMoved(camera,previous) || s.animationMoved) accumulated=0;
    FrameConstants c{}; c.motionNdc=o.motion=="ndc" ? 1u : 0u; c.currentVP=Mul(Perspective(o.fov,float(width)/float(height),o.reverse),LookAt(camera.eye,camera.target));
    c.previousVP=initialized ? previousVP : c.currentVP;
    c.eye=camera.eye; c.tanHalf=std::tan(o.fov*3.14159265f/360); c.forward=Normalize(camera.target-camera.eye);
    c.right=Normalize(Cross({0,1,0},c.forward)); c.up=Cross(c.forward,c.right); c.aspect=float(width)/float(height);
    c.exposure=o.exposure; c.albedo=o.albedo; c.sunTravel=Normalize(o.sunDir); c.sunStrength=o.sunStrength;
    c.width=width; c.height=height; c.frame=unsigned(frame); c.spp=unsigned(o.spp); c.bounces=unsigned(o.bounces);
    c.opaqueTriangles=s.opaqueTriangles; c.lightCount=s.lightCount;
    c.dynamicBase=s.staticTriangles; c.dynamicOpaque=s.dynamicOpaqueTriangles;
    const bool averaging=o.accumulationEnabled || o.view=="accum" || !o.interactive;
    if(!averaging) accumulated=0;
    c.accumulation=accumulated;
    c.accumulationLimit=o.accumulationEnabled && !o.accumulationInfinite ? unsigned(o.accumulationFrames) : UINT_MAX;
    c.padding=o.accumulationEnabled && o.feedAccumulation ? 1u : 0u;
    c.pickX=pickX<0 ? -1 : std::clamp(int(float(pickX)*float(width)/float(d.width)),0,int(width)-1);
    c.pickY=pickY<0 ? -1 : std::clamp(int(float(pickY)*float(height)/float(d.height)),0,int(height)-1);
    c.jitterX=o.jitter ? Halton(unsigned(frame)+1,2)-0.5f : 0; c.jitterY=o.jitter ? Halton(unsigned(frame)+1,3)-0.5f : 0;
    c.firefly=o.firefly; c.sunCos=std::cos(o.sunAngle*0.5f*3.14159265f/180); c.lightPower=s.lightPower; c.reverse=o.linearDepth<0 ? 2u : o.linearDepth>0 ? 3u : o.reverse ? 1u : 0u;
    c.view=o.view=="albedo" ? 1u : o.view=="normal" ? 2u : o.view=="depth" ? 3u : o.view=="motion" ? 4u : o.view=="accum" ? 5u : o.view=="roughness" ? 6u : o.view=="specular" ? 7u : 0u;
    c.lightCandidates=unsigned(o.lightCandidates);
    c.hazeDensity=o.haze; c.hazeG=o.hazeG; c.bloom=o.bloom; c.neutralTonemap=o.tonemap=="neutral" ? 1u : 0u;
    void *p=nullptr; D3D12_RANGE empty{0,0}; Check(constants->Map(0,&empty,&p),"Map frame constants"); std::memcpy(p,&c,sizeof(c)); constants->Unmap(0,nullptr);
    d.Begin(); d.Timestamp(0); ID3D12DescriptorHeap *heaps[]={d.heap.Get()}; d.list->SetDescriptorHeaps(1,heaps);
    s.CopyAnimation(d); a.Update(d,unsigned(o.blasRebuild),s.animationRefitNeeded);
    if(initialized) for(auto &r:outputs) Transition(d.list.Get(),r.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    d.list->SetComputeRootSignature(traceRoot.Get()); d.list->SetPipelineState(tracePso.Get());
    d.list->SetComputeRootConstantBufferView(0,constants->GetGPUVirtualAddress());
    d.list->SetComputeRootShaderResourceView(1,a.tlas->GetGPUVirtualAddress());
    d.list->SetComputeRootDescriptorTable(2,d.Gpu(s.srvBase)); d.list->SetComputeRootDescriptorTable(3,d.Gpu(uavBase));
    d.list->SetComputeRootShaderResourceView(4,s.groupBuffer->GetGPUVirtualAddress());
    d.Timestamp(1); d.list->Dispatch((width+7)/8,(height+7)/8,1); d.Timestamp(2);
    if(pickX>=0) {
        Transition(d.list.Get(),pickBuffer.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
        d.list->CopyBufferRegion(pickReadback.Get(),0,pickBuffer.Get(),0,8);
        Transition(d.list.Get(),pickBuffer.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        pickPending=true; pickX=pickY=-1;
    }
    if(ngx.Output()) {
        NgxFrame f; for(unsigned i=0;i<7;++i) f.inputs[i]=outputs[i].Get();
        if(o.accumulationEnabled && o.feedAccumulation) f.inputs[0]=outputs[8].Get();
        f.width=width; f.height=height; f.jitterX=c.jitterX; f.jitterY=c.jitterY;
        f.mvScaleX=o.motion=="ndc" ? -0.5f*float(width) : 1.0f;
        f.mvScaleY=o.motion=="ndc" ? 0.5f*float(height) : 1.0f;
        f.deltaMilliseconds=frameDelta*1000;
        f.reset=resetHistory || !initialized || CameraCut(camera,previous);
        f.worldToView=NgxMatrix(LookAt(camera.eye,camera.target));
        f.viewToClip=NgxMatrix(Perspective(o.fov,c.aspect,o.reverse));
        ngx.Evaluate(d,f);
        // NGX may replace descriptor heaps and root signatures on this command list.
        d.list->SetDescriptorHeaps(1,heaps);
    }
    d.Timestamp(4);
    for(auto &r:outputs) Transition(d.list.Get(),r.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    display.Prepare(d,DisplaySource(o),o.autoExposure);
    Transition(d.list.Get(),d.BackBuffer(),D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_RENDER_TARGET);
    d.list->SetGraphicsRootSignature(presentRoot.Get()); d.list->SetPipelineState(presentPso.Get());
    d.list->SetGraphicsRootConstantBufferView(0,constants->GetGPUVirtualAddress()); d.list->SetGraphicsRootDescriptorTable(1,d.Gpu(srvBase));
    auto rtv=d.BackRtv(); d.list->OMSetRenderTargets(1,&rtv,FALSE,nullptr);
    D3D12_VIEWPORT viewport{0,0,float(d.width),float(d.height),0,1}; D3D12_RECT scissor{0,0,LONG(d.width),LONG(d.height)};
    d.list->RSSetViewports(1,&viewport); d.list->RSSetScissorRects(1,&scissor); d.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    d.list->DrawInstanced(3,1,0,0);
    if(overlay) overlay(d);
    // Measure rendering including NGX/presentation, excluding optional readback and vsync wait.
    d.Timestamp(3); d.ResolveTimings();
    if(dump) d.QueueDump();
    Transition(d.list.Get(),d.BackBuffer(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PRESENT); d.Submit(true);
    d.CollectTimings();
    if(dump) d.WriteDump(frame);
    previous=camera; previousVP=c.currentVP; initialized=true; resetHistory=false;
    if(averaging && accumulated<c.accumulationLimit) ++accumulated;
}
