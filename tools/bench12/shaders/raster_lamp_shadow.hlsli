#ifndef PW_RASTER_LAMP_SHADOW_HLSLI
#define PW_RASTER_LAMP_SHADOW_HLSLI
// --lamp-shadows (PW_LAMPSHADOW, raster_lamp_shadows.h, raster_lamp_shadow_passes.cpp): a spot shadow map per lamp group,
// one slice of a Texture2DArray each, drawn every frame. Included by raster_common.hlsli (after LampFalloff) and, for the
// shadow pass's vertex shader, by raster_shadow.hlsl. Space 7 is used by no other root:
//   b0 the frusta (written every frame: the weights follow the menu's lamp intensity), t1 the maps of every caster,
//   t2 the maps of the casters without baked light (--lightmap only; a null view otherwise), t3 the slice of each raster
//   light (rasterLights[i] at i, rasterLines[k] at rasterLightCount + k; 0xffffffff: unshadowed).
// The shadow pass's root adds b1 space7 (the slice drawn) and the triangle list at t2 space3 (raster_shadow.hlsl).
static const uint MaxLampShadows=16; // raster_lamp_shadows.h
static const uint NoLampShadow=0xffffffff;
cbuffer RasterLampShadows : register(b0,space7) {
    row_major float4x4 lampShadowVP[MaxLampShadows]; // LampShadowFrustum::viewProjection
    float4 lampShadowApex[MaxLampShadows];   // xyz the apex, w texelScale (one texel per metre of view depth)
    float4 lampShadowCentre[MaxLampShadows]; // xyz the group's power-weighted centroid, w its luminance power (the weights)
    uint lampShadowCount; uint lampAtlasBase; float2 lampShadowPadding; // lampAtlasBase: the first lamp map in lightmapAtlases[]
};
Texture2DArray<float> lampShadowMaps : register(t1,space7);
Texture2DArray<float> lampShadowDynamicMaps : register(t2,space7);
StructuredBuffer<uint> lampShadowSlices : register(t3,space7);

// The visibility of slice `g`'s apex from a surface point (1 lit, 0 shadowed): 3x3 PCF as the sun's, with a normal offset
// of 1.5 texels and the lookup point moved one texel toward the apex (texels at the point's view depth), on top of the
// pass's slope-scaled bias. Outside the frustum: lit. A surface facing away from the apex is not tested (1), and the test
// fades in up to a cosine of 0.2: one map stands for a whole group, and a surface turned away from its apex (a character's
// back under a field of tubes) is lit by the group's other emitters, which the map cannot see.
float LampShadowPcf(Texture2DArray<float> maps, uint g, float3 position, float3 geometric)
{
    float3 toApex=lampShadowApex[g].xyz-position;
    float depth=toApex.y; // the view depth along the maps' axis (0, -1, 0): the apex lies above the point
    if(depth<=0) return 1;
    float3 l=normalize(toApex);
    float facing=dot(geometric,l);
    if(facing<=0) return 1;
    float texel=depth*lampShadowApex[g].w;
    float4 clip=mul(lampShadowVP[g],float4(position+geometric*(1.5*texel)+l*texel,1));
    if(clip.w<=1e-5) return 1;
    float3 s=float3(clip.xy/clip.w*float2(0.5,-0.5)+0.5,clip.z/clip.w);
    if(any(s.xy<0) || any(s.xy>1) || s.z<0 || s.z>1) return 1;
    float sum=0;
    [unroll] for(int y=-1;y<=1;++y) [unroll] for(int x=-1;x<=1;++x) sum+=maps.SampleCmpLevelZero(shadowSampler,float3(s.xy,g),s.z,int2(x,y));
    return lerp(1,sum/9,saturate(facing/0.2));
}
// Every group's visibility at the surface, from the maps of every caster (the raster lights' shadows).
void LampShadowsAt(Surface s, out float visibility[MaxLampShadows])
{
    [unroll] for(uint k=0;k<MaxLampShadows;++k) visibility[k]=1;
    [loop] for(uint g=0;g<lampShadowCount;++g) visibility[g]=LampShadowPcf(lampShadowMaps,g,s.position,s.geometric);
}
float LampSliceVisibility(float visibility[MaxLampShadows], uint slice) { return slice<lampShadowCount ? visibility[slice] : 1; }
// The baked mode's lamp visibility on a static surface: each group's test against the maps of the casters without baked
// light, averaged with the group's luminance power times its point light's falloff at the surface as the weight (1 when
// nothing weighs). The bake's lamp map holds the static shadows; this darkens it where a character or foliage stands
// between the surface and a group. LampVisibility in raster_lamp_shadows.cpp mirrors it.
float LampVisibility(Surface s)
{
    float sum=0, weight=0;
    [loop] for(uint g=0;g<lampShadowCount;++g) {
        float w=lampShadowCentre[g].w*LampFalloff(lampShadowCentre[g].xyz-s.position);
        if(w<=0) continue;
        sum+=w*LampShadowPcf(lampShadowDynamicMaps,g,s.position,s.geometric); weight+=w;
    }
    return weight>0 ? sum/weight : 1;
}
#define PW_LAMP_VISIBILITY(index) *LampSliceVisibility(lampVisibility,lampShadowSlices[index])
#endif
