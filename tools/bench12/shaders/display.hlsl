#include "display_math.hlsli"
cbuffer Display : register(b0) {
    uint2 inputSize, originalSize;
    uint span, firstLevel, resetExposure, autoExposure;
    float adaptSeconds, speedUp, speedDown, evScale; // Display::Prepare's ExposureControl (evScale: 2^ev)
    float centre; uint metering; float minScale, maxScale; // Histogram: the centre weighting (0: flat); Adapt: 1 UE metering
    float meterExposure, curveLow, curveHigh, curveBias;   // UE metering: --exposure, --exposure-curve
    uint replay; float replayScale;                        // --exposure-replay: the recorded scale (exposure_track.h)
};
Texture2D<float4> source : register(t0);
RWTexture2D<float4> destination : register(u0);
// The metering histogram (METER_BINS weights), then the last exposure, its average-based and highlight-based targets
// (floats as bits, read back by Display::LastExposure).
RWStructuredBuffer<uint> meter : register(u1);
[numthreads(8,8,1)]
void Downsample(uint3 id : SV_DispatchThreadID)
{
    uint2 size=(inputSize+1)/2;
    if(any(id.xy>=size)) return;
    float4 sum=0; float total=0;
    for(uint y=0;y<2;++y) for(uint x=0;x<2;++x) {
        uint2 p=id.xy*2+uint2(x,y);
        if(any(p>=inputSize)) continue;
        float4 value=source.Load(int3(p,0));
        if(firstLevel) {
            value.rgb=max(value.rgb,0);
            value.a=log(max(dot(value.rgb,float3(0.2126,0.7152,0.0722)),1e-5));
        }
        // Exact log average even for odd image sizes: edge texels represent fewer pixels.
        uint2 covered=min(span.xx,originalSize-p*span);
        float weight=float(covered.x)*covered.y;
        sum+=value*weight; total+=weight;
    }
    destination[id.xy]=sum/total;
}
// One pyramid level (inputSize texels) into the histogram, each texel weighted 1 at the centre down to 1 - centre at
// the corners (in 1/16 steps, at least 1/16).
[numthreads(8,8,1)]
void Histogram(uint3 id : SV_DispatchThreadID)
{
    if(any(id.xy>=inputSize)) return;
    float luminance=dot(source.Load(int3(id.xy,0)).rgb,float3(0.2126,0.7152,0.0722));
    float2 offset=((float2(id.xy)+0.5)/float2(inputSize)-0.5)*2;
    float weight=1-centre*saturate(length(offset)/1.4142);
    InterlockedAdd(meter[MeterBin(luminance)],max(1u,uint(round(weight*16))));
}
groupshared uint bins[METER_BINS];
[numthreads(METER_BINS,1,1)]
void Adapt(uint3 id : SV_DispatchThreadID)
{
    bins[id.x]=meter[id.x]; meter[id.x]=0; // consumed: the next frame's histogram starts empty
    GroupMemoryBarrierWithGroupSync();
    if(id.x!=0) return;
    float logAverage=source.Load(int3(0,0,0)).a, highlight=MeterPercentile(bins,HIGHLIGHT_PERCENTILE);
    float band=MeterBandAverage(bins,0.1,0.9);
    float curve=exp2(ExposureCurveBias(SceneEv100(band,meterExposure),curveLow,curveHigh,curveBias));
    float target=metering!=0 ? UeMeterTarget(band,evScale*curve,minScale,maxScale) : MeterTarget(logAverage,highlight,evScale);
    // A reset (the first frame, a new size) takes the target at once, as a game does on a level load.
    float previous=resetExposure ? target : destination[uint2(0,0)].r;
    float scale=autoExposure ? (resetExposure ? target : AdaptExposure(previous,target,adaptSeconds,speedUp,speedDown)) : 1;
    if(replay!=0) scale=replayScale; // the recorded run's scale of this frame, bit for bit
    destination[uint2(0,0)]=float4(scale,0,0,1);
    meter[METER_BINS]=asuint(scale); meter[METER_BINS+1]=asuint(metering!=0 ? 0.18/exp2(band)*evScale : 0.18/exp(logAverage)*evScale);
    meter[METER_BINS+2]=asuint(HIGHLIGHT_KEY/exp2(highlight)*evScale);
}
