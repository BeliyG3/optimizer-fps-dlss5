#include "pathtrace.h"
#include "ngx_nr_setting_at.h"
#include "raw_dump.h"
#include <cstring>
#include "startup_profile.h"

Pathtrace::Pathtrace(Device &d)
{
    CreatePipelines(d);
    constants=d.Buffer(512,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
    uavBase=d.Allocate(10); srvBase=d.Allocate(14); // srvBase+8..13: Display's present views
    pickBuffer=d.Buffer(8,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    pickReadback=d.Buffer(8,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_UNORDERED_ACCESS_VIEW_DESC u{}; u.ViewDimension=D3D12_UAV_DIMENSION_BUFFER;
    u.Buffer.NumElements=1; u.Buffer.StructureByteStride=8;
    d.gpu->CreateUnorderedAccessView(pickBuffer.Get(),nullptr,&u,d.Cpu(uavBase+8));
    raster.Create(d);
}
void Pathtrace::Render(Device &d, const Scene &s, Accel &a, Options &o, int frame, bool dump, const CameraState *pose, void (*overlay)(Device &),float frameDelta,float exposureSeconds)
{
    Configure(d,o);
    if(frame==o.nrRecreate) nr.Recreate(d,frame,o.nrReleaseThread);
    for(const auto &set:o.setAt) if(frame==set.frame) SetAddonSettingAt(frame,std::uint32_t(set.id),set.value);
    nr.Prepare(d,frame);
    CameraState camera=pose ? *pose : CameraAt(frame,o,s.cameraAnchor);
    if(!initialized || rewound || CameraMoved(camera,previous) || s.animationMoved) accumulated=0;
    FrameConstants c{}; c.motionNdc=o.motion=="ndc" ? 1u : 0u; c.currentVP=Mul(Perspective(o.fov,float(width)/float(height),o.reverse),LookAt(camera.eye,camera.target));
    c.previousVP=initialized ? previousVP : c.currentVP;
    if((!initialized || rewound) && frame>0 && !pose) { // --start-frame S, --variants: frame S-1's view, as in the run from 0
        const CameraState before=CameraAt(frame-1,o,s.cameraAnchor);
        c.previousVP=Mul(Perspective(o.fov,float(width)/float(height),o.reverse),LookAt(before.eye,before.target));
    }
    if(o.SpanAt(frame)>1) { // --motion-span K: frame t-K's view (CameraAt is a function of the frame; follow/interactive are rejected)
        const CameraState from=CameraAt(std::max(frame-o.SpanAt(frame),0),o,s.cameraAnchor);
        c.previousVP=Mul(Perspective(o.fov,float(width)/float(height),o.reverse),LookAt(from.eye,from.target));
    }
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
    if(o.render=="raster" && o.upscaler=="none") c.jitterX=c.jitterY=0; // games without an upscaler do not jitter
    c.firefly=FireflyLimit(o.firefly,o.autoExposure,exposureTrack.Replaying() ? exposureTrack.At(frame-1) : display.LastExposure()); c.sunCos=std::cos(o.sunAngle*0.5f*3.14159265f/180); c.lightPower=s.lightPower; c.reverse=o.linearDepth<0 ? 2u : o.linearDepth>0 ? 3u : o.reverse ? 1u : 0u;
    c.view=o.view=="albedo" ? 1u : o.view=="normal" ? 2u : o.view=="depth" ? 3u : o.view=="motion" ? 4u : o.view=="accum" ? 5u : o.view=="roughness" ? 6u : o.view=="specular" ? 7u : o.view=="vegmask" ? 8u : 0u;
    if(o.DumpsRaw("prevdepth")) c.view=9u; // the motion guide carries the span-back view depth (raw_dump.h)
    c.lightCandidates=unsigned(o.lightCandidates);
    c.hazeDensity=o.haze; c.hazeG=o.hazeG; c.bloom=o.bloom;
    c.neutralTonemap=o.tonemap=="neutral" ? 1u : o.tonemap=="none" ? 2u : o.tonemap=="ue-filmic" ? 3u : 0u;
    c.filmSlope=o.filmSlope; c.filmToe=o.filmToe; c.filmShoulder=o.filmShoulder; c.filmBlackClip=o.filmBlackClip; c.filmWhiteClip=o.filmWhiteClip;
    c.localExposure=o.localExposure ? 1u : 0u; c.localHighlights=o.localHighlights; c.localShadows=o.localShadows;
    c.localDetail=o.localDetail; c.localBlend=o.localBlend;
    void *p=nullptr; D3D12_RANGE empty{0,0}; Check(constants->Map(0,&empty,&p),"Map frame constants"); std::memcpy(p,&c,sizeof(c)); constants->Unmap(0,nullptr);
    d.Begin(); d.Timestamp(0); ID3D12DescriptorHeap *heaps[]={d.heap.Get()}; d.list->SetDescriptorHeaps(1,heaps);
    d.Mark("start"); s.RecordAnimation(d); d.Mark("animation"); // both renderers read the animated vertices; only the path tracer reads the structures
    if(o.render=="raster") a.Skip(d,s.animationRefitNeeded); else a.Update(d,unsigned(o.blasRebuild),s.animationRefitNeeded);
    d.Mark("blas and tlas");
    if(initialized) for(auto &r:outputs) Transition(d.list.Get(),r.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if(o.render=="raster") {
        d.Timestamp(1); raster.Record(d,s,o,c,constants->GetGPUVirtualAddress(),outputs,uavBase); d.Timestamp(2);
    } else {
        d.list->SetComputeRootSignature(traceRoot.Get()); d.list->SetPipelineState(tracePso.Get());
        d.list->SetComputeRootConstantBufferView(0,constants->GetGPUVirtualAddress());
        d.list->SetComputeRootShaderResourceView(1,a.tlas->GetGPUVirtualAddress());
        d.list->SetComputeRootDescriptorTable(2,d.Gpu(s.srvBase)); d.list->SetComputeRootDescriptorTable(3,d.Gpu(uavBase));
        d.list->SetComputeRootShaderResourceView(4,s.groupBuffer->GetGPUVirtualAddress());
        d.Timestamp(1); d.list->Dispatch((width+7)/8,(height+7)/8,1); d.Timestamp(2); d.Mark("path trace");
    }
    if(o.motion=="none") raster.ZeroMotion(d,uavBase,width,height);
    if(o.hud) {
        hud.Record(d,outputs[0].Get(),uavBase,width,height,frame);
        // "Feed accumulated image" hands NGX outputs[8] (u9 of the table) instead: it gets the HUD as well.
        if(o.accumulationEnabled && o.feedAccumulation) hud.Record(d,outputs[8].Get(),uavBase+9,width,height,frame);
    }
    if(pickX>=0) {
        Transition(d.list.Get(),pickBuffer.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
        d.list->CopyBufferRegion(pickReadback.Get(),0,pickBuffer.Get(),0,8);
        Transition(d.list.Get(),pickBuffer.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        pickPending=true; pickX=pickY=-1;
    }
    const float mvScaleX=o.motion=="ndc" ? -0.5f*float(width) : 1.0f, mvScaleY=o.motion=="ndc" ? 0.5f*float(height) : 1.0f;
    const bool historyReset=resetHistory || !initialized || CameraCut(camera,previous);
    // --mv-format rgba16f: DLSS and NR read an RGBA16F copy of the motion guide.
    ID3D12Resource *motion=motionPack.Target() && (ngx.Output() || nr.Active()) ? motionPack.Run(d) : outputs[2].Get();
    if(ngx.Output()) {
        NgxFrame f; for(unsigned i=0;i<7;++i) f.inputs[i]=outputs[i].Get();
        if(o.accumulationEnabled && o.feedAccumulation) f.inputs[0]=outputs[8].Get();
        f.inputs[2]=motion;
        f.width=width; f.height=height; f.jitterX=c.jitterX; f.jitterY=c.jitterY;
        f.mvScaleX=mvScaleX; f.mvScaleY=mvScaleY;
        f.deltaMilliseconds=frameDelta*1000;
        f.reset=historyReset;
        f.worldToView=NgxMatrix(LookAt(camera.eye,camera.target));
        f.viewToClip=NgxMatrix(Perspective(o.fov,c.aspect,o.reverse));
        ngx.Evaluate(d,f); d.Mark("ngx (dlss sr/rr)");
        // NGX may replace descriptor heaps and root signatures on this command list.
        d.list->SetDescriptorHeaps(1,heaps);
    }
    if(nr.Active()) {
        // Feature 18 follows the upscaler: its colour is the SR/RR output (a pixel SRV after NGX) or
        // the render colour (still a UAV); depth and motion are the render-size guides.
        NrInputs in;
        in.colourState=ngx.Output() ? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        in.depth=outputs[1].Get(); in.motion=motion; in.mvScaleX=mvScaleX; in.mvScaleY=mvScaleY; in.reset=historyReset || o.NrResetAt(frame);
        nr.Evaluate(d,in,frame,o); d.Mark("nr");
        d.list->SetDescriptorHeaps(1,heaps);
    }
    d.Timestamp(4);
    for(auto &r:outputs) Transition(d.list.Get(),r.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    BindDisplay(d,o);
    ExposureControl exposure{o.autoExposure,exposureSeconds,o.exposureSpeedUp,o.exposureSpeedDown,o.exposureEv,o.exposureCentre,
                             o.exposureMetering=="ue",o.localExposure};
    MeterScaleBounds(o.exposureMinEv,o.exposureMaxEv,o.exposure,exposure.minScale,exposure.maxScale);
    exposure.exposure=o.exposure; exposure.curveLow=o.exposureCurve.x; exposure.curveHigh=o.exposureCurve.y; exposure.curveBias=o.exposureCurve.z;
    if(exposureTrack.Replaying()) { exposure.replay=true; exposure.replayScale=exposureTrack.At(frame).scale; }
    display.Prepare(d,DisplaySource(o),exposure);
    Transition(d.list.Get(),d.BackBuffer(),D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_RENDER_TARGET);
    d.list->SetGraphicsRootSignature(presentRoot.Get()); d.list->SetPipelineState(presentPso.Get());
    d.list->SetGraphicsRootConstantBufferView(0,constants->GetGPUVirtualAddress()); d.list->SetGraphicsRootDescriptorTable(1,d.Gpu(srvBase));
    auto rtv=d.BackRtv(); d.list->OMSetRenderTargets(1,&rtv,FALSE,nullptr);
    D3D12_VIEWPORT viewport{0,0,float(d.width),float(d.height),0,1}; D3D12_RECT scissor{0,0,LONG(d.width),LONG(d.height)};
    d.list->RSSetViewports(1,&viewport); d.list->RSSetScissorRects(1,&scissor); d.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    d.list->DrawInstanced(3,1,0,0);
    if(overlay) overlay(d);
    d.Mark("display and present");
    // Measure rendering including NGX/presentation, excluding optional readback and vsync wait.
    d.Timestamp(3); d.ResolveTimings();
    if(dump) d.QueueDump();
    RawDump rawMotion, rawDepth; const bool rawMotionDump=dump && (o.DumpsRaw("motion") || o.DumpsRaw("prevdepth")), rawDepthDump=dump && o.DumpsRaw("depth");
    if(rawMotionDump) rawMotion.Queue(d,outputs[2].Get()); // the render-size motion guide, before the add-on's packing
    if(rawDepthDump) rawDepth.Queue(d,outputs[1].Get());   // the render-size depth guide (R32_FLOAT)
    Transition(d.list.Get(),d.BackBuffer(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PRESENT); d.Submit(true);
    d.CollectTimings();
    if(exposureTrack.Recording()) exposureTrack.Write(frame,display.LastExposure()); // Submit waited: this frame's
    const double writeStart=dump ? StartupProfile::Get().Now() : 0;
    if(dump) d.WriteDump(frame,o.dumpFormat=="png");
    if(rawMotionDump) rawMotion.Write(frame,o.DumpsRaw("prevdepth"));
    if(rawDepthDump) rawDepth.Write(frame);
    if(dump) dumpWriteSeconds+=StartupProfile::Get().Now()-writeStart;
    if(dump && o.autoExposure) {
        const auto e=display.LastExposure();
        std::printf("[exposure] frame %d: auto scale %.5g (--exposure %g x scale = %.5g), targets: average %.5g, highlights %.5g\n",
                    frame,double(e.scale),double(o.exposure),double(o.exposure*e.scale),double(e.average),double(e.highlight));
    }
    previous=camera; previousVP=c.currentVP; initialized=true; resetHistory=false; rewound=false;
    if(averaging && accumulated<c.accumulationLimit) ++accumulated;
}
