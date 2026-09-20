#include "display_math.hlsli"
cbuffer Display : register(b0) {
    uint2 inputSize, originalSize;
    uint span, firstLevel, resetExposure, autoExposure;
};
Texture2D<float4> source : register(t0);
RWTexture2D<float4> destination : register(u0);
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
[numthreads(1,1,1)]
void Adapt(uint3 id : SV_DispatchThreadID)
{
    float previous=1;
    if(!resetExposure) previous=destination[uint2(0,0)].r;
    float scale=autoExposure ? AdaptExposure(previous,source.Load(int3(0,0,0)).a,1.0/60) : 1;
    destination[uint2(0,0)]=float4(scale,0,0,1);
}
