#ifndef PW_RASTER_CSM_HLSLI
#define PW_RASTER_CSM_HLSLI
// --render raster --csm: the sun shadow in four cascades (raster_csm.h fits them, raster_csm.cpp draws them). Only
// the shaders compiled with PW_CSM include this; the CSM root signature binds it (raster_pipeline.cpp).
#include "raster_common.hlsli"
// b2 space1, not b2: --lightmap's RasterLightmap root constants hold b2 (space0) in the baked CSM variant.
cbuffer RasterCascades : register(b2,space1) {
    row_major float4x4 cascadeVP[4]; // FitCascades, the layout of sunVP
    float4 cascadeSplitFar;          // view depth of each slice's far end
    float4 cascadeTexelWorld;        // one texel of each cascade, in scene units
    float4 cascadeBias;              // depth bias of each cascade: one of its texels, in its depth units
    float3 cascadeForward;           // the unjittered camera's forward: view depth = dot(position - eye, forward)
    float cascadeBlend;              // blend band, a fraction of each slice
};
Texture2DArray<float> shadowCascades : register(t3,space2); // slice c: cascade c, drawn with every caster

// 3x3 PCF of cascade c, as ShadowMapPcf. The depth bias is one texel of cascade c in world units, as the single map's
// is one of its texels (Raster::WriteConstants): texelWorld / depthRange, computed per cascade on the CPU, so it does
// not change with the cascade's depth range from frame to frame.
float CsmPcf(Texture2DArray<float> maps, float3 position, uint c)
{
    float4 clip=mul(cascadeVP[c],float4(position,1));
    float3 s=float3(clip.xy*float2(0.5,-0.5)+0.5,clip.z-cascadeBias[c]);
    if(any(s.xy<0) || any(s.xy>1)) return 1;
    float sum=0;
    [unroll] for(int y=-1;y<=1;++y) [unroll] for(int x=-1;x<=1;++x)
        sum+=maps.SampleCmpLevelZero(shadowSampler,float3(s.xy,c),s.z,int2(x,y));
    return sum/9;
}
// viewDepth: distance along the camera forward. Beyond the last split: lit, as outside the single map.
float CsmSunVisibility(Texture2DArray<float> maps, float3 position, float viewDepth)
{
    uint c=0; [unroll] for(uint k=0;k<3;++k) c+=viewDepth>cascadeSplitFar[k] ? 1 : 0;
    if(viewDepth>cascadeSplitFar[3]) return 1;
    float v=CsmPcf(maps,position,c);
    float start=c==0 ? 0 : cascadeSplitFar[c-1], band=(cascadeSplitFar[c]-start)*cascadeBlend;
    float t=saturate((viewDepth-(cascadeSplitFar[c]-band))/max(band,1e-4));
    if(c<3 && t>0) v=lerp(v,CsmPcf(maps,position,c+1),t);
    return v;
}
float CsmViewDepth(float3 position) { return dot(position-eye,cascadeForward); }
// SurfaceSunVisibility (raster_common.hlsli) with the cascades instead of the single map: the pixel's one sun
// visibility, read by its sun light and its fog.
float CsmSurfaceSunVisibility(Surface s)
{
    if(sunStrength<=0 || dot(s.geometric,-sunTravel)<=0) return 0;
    return CsmSunVisibility(shadowCascades,s.position+s.geometric*shadowNormalOffset,CsmViewDepth(s.position));
}
#endif
