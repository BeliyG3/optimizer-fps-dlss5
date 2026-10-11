// --render raster, resolve: writes the SAME outputs the path tracer's CS fills (u0..u9, the trace's layout) from
// the raster targets. Sky where the depth buffer kept its clear value, analytic fog everywhere, the trace's depth
// convention, firefly clamp and accumulation. ZeroMotion is --motion none of both render modes.
// PW_FXAA=1 (--fxaa, raster_fxaa.hlsli) filters the finished colour before it is written; without it this file builds as before.
#include "raster_common.hlsli"
RWTexture2D<float4> colour : register(u0);
RWTexture2D<float> depth : register(u1);
RWTexture2D<float2> motion : register(u2);
RWTexture2D<float4> normalRoughness : register(u3);
RWTexture2D<float4> diffuseAlbedo : register(u4);
RWTexture2D<float4> specularAlbedo : register(u5);
RWTexture2D<float> specHitDistance : register(u6);
RWTexture2D<float4> accumulated : register(u7);
RWStructuredBuffer<uint2> pickResult : register(u8);
RWTexture2D<float4> accumulatedInput : register(u9);
Texture2D<float4> colourTarget : register(t2,space2);
Texture2D<float> depthTarget : register(t3,space2);
Texture2D<float2> motionTarget : register(t4,space2);
#if PW_FXAA
#include "raster_fxaa.hlsli" // --fxaa: Fxaa() below, from the targets above
#endif

[numthreads(8,8,1)]
void Resolve(uint3 dispatch : SV_DispatchThreadID)
{
    uint2 pixel=dispatch.xy; if(pixel.x>=width || pixel.y>=height) return;
    // The trace's primary ray for this pixel (jittered), so sky and fog match the rasterised sample position.
    float2 ndc=(float2(pixel)+0.5-float2(jitterX,jitterY))/float2(width,height)*2-1;
    float3 direction=normalize(cameraForward+cameraRight*(ndc.x*aspect*tanHalf)-cameraUp*(ndc.y*tanHalf));
    float forwardCos=dot(direction,cameraForward);
    // reverseDepth 1 (reverse) and 2 (linear-negative) use the reversed projection; 0 and 3 the standard one.
    bool reverse=reverseDepth==1 || reverseDepth==2;
    float z=depthTarget[pixel];
    bool hit=z!=(reverse ? 0.0 : 1.0);
    float3 sum; float fogDistance, sunVisibility=1; // the sky keeps the sun's in-scatter
    if(hit) {
        float viewDepth=ViewDepthFromHardware(z,reverse);
        depth[pixel]=reverseDepth==2 ? -viewDepth : reverseDepth==3 ? viewDepth : saturate(z);
        motion[pixel]=motionTarget[pixel];
        float4 shaded=colourTarget[pixel]; // alpha: the surface's sun visibility (raster.hlsl)
        sum=shaded.rgb; sunVisibility=shaded.a; fogDistance=min(viewDepth/forwardCos,300);
    } else {
        depth[pixel]=reverseDepth==2 ? -300.0 : reverseDepth==3 ? 300.0 : reverseDepth!=0 ? 0 : 1;
        motion[pixel]=0;
        sum=max(Sky(direction)*exposure,0);
        if(!all(isfinite(sum))) sum=0;
        fogDistance=300;
    }
    normalRoughness[pixel]=0; diffuseAlbedo[pixel]=0; specularAlbedo[pixel]=0; specHitDistance[pixel]=0;
    // Raster mode has no triangle picking: report "nothing" rather than a stale pick.
    if(pickX>=0 && all(pixel==uint2(pickX,pickY))) pickResult[0]=uint2(0xffffffffu,0xffffffffu);
    sum=sum*MediumTransmittance(hazeDensity,fogDistance)+RasterFog(direction,fogDistance,sunVisibility)*exposure;
    if(!all(isfinite(sum))) sum=0;
    sum*=min(1,firefly/max(Luminance(sum),1e-8));
#if PW_FXAA
    sum=Fxaa(pixel,sum); // --fxaa: the anti-aliased frame is what is written, accumulated and fed on
#endif
    colour[pixel]=float4(sum,1);
    if(accumulation==0) accumulated[pixel]=float4(sum,1);
    else if(accumulation<accumulationLimit) accumulated[pixel]=float4(lerp(accumulated[pixel].rgb,sum,1.0/(accumulation+1)),1);
    if(feedAccumulation!=0) accumulatedInput[pixel]=accumulated[pixel];
}
[numthreads(8,8,1)]
void ZeroMotion(uint3 dispatch : SV_DispatchThreadID)
{
    uint w,h; motion.GetDimensions(w,h);
    if(dispatch.x<w && dispatch.y<h) motion[dispatch.xy]=0;
}
