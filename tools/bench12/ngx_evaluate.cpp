#include "ngx.h"

void Ngx::Evaluate(Device &d, NgxFrame &f)
{
    const unsigned count=rr ? 7u : 3u;
    for(unsigned i=0;i<count;++i)
        Transition(d.list.Get(),f.inputs[i],D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if(evaluated) Transition(d.list.Get(),output.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    f.output=output.Get(); AuditNgxParameters(!evaluated);
    stateSentinel.Before(d);
    NVSDK_NGX_Result result;
    if(rr) {
        auto p=RrEvaluate(f);
        result=NGX_D3D12_EVALUATE_DLSSD_EXT(d.list.Get(),feature,parameters,&p);
    } else {
        NVSDK_NGX_D3D12_DLSS_Eval_Params p{};
        p.Feature.pInColor=f.inputs[0]; p.Feature.pInOutput=f.output;
        p.pInDepth=f.inputs[1]; p.pInMotionVectors=f.inputs[2];
        p.InJitterOffsetX=f.jitterX; p.InJitterOffsetY=f.jitterY; p.InMVScaleX=f.mvScaleX; p.InMVScaleY=f.mvScaleY;
        p.InRenderSubrectDimensions={f.width,f.height}; p.InReset=f.reset ? 1 : 0;
        p.InFrameTimeDeltaInMsec=f.deltaMilliseconds; p.InPreExposure=1; p.InExposureScale=1;
        result=NGX_D3D12_EVALUATE_DLSS_EXT(d.list.Get(),feature,parameters,&p);
    }
    AuditNgxParameters(false); CheckNgx(result,rr ? "DLSS RR evaluate" : "DLSS SR evaluate");
    stateSentinel.After(d);
    for(unsigned i=0;i<count;++i)
        Transition(d.list.Get(),f.inputs[i],D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Transition(d.list.Get(),output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    std::printf("[ngx frame] reset=%d jitter=(%.6f,%.6f) render=%ux%u dt=%.6f ms\n",
        f.reset ? 1 : 0,double(f.jitterX),double(f.jitterY),f.width,f.height,double(f.deltaMilliseconds));
    evaluated=true;
}
