#ifndef PW_RASTER_LIGHTMAP_HLSLI
#define PW_RASTER_LIGHTMAP_HLSLI
// --render raster --lightmap: baked lighting of static geometry (lightmap.h, raster_lightmap.cpp). Only the shaders
// compiled with PW_LIGHTMAP include this; the baked root signature binds it.
#include "raster_common.hlsli"
struct LightmapUv { float2 uv; float atlas; float padding; }; // lightmap_uv.h, one per vertex of `vertices`
StructuredBuffer<LightmapUv> lightmapUvs : register(t0,space3);
#if PW_CSM
#include "raster_csm.hlsli"
Texture2DArray<float> dynamicShadowMap : register(t1,space3); // --csm: the dynamic triangles alone, in the cascades
#else
Texture2D<float> dynamicShadowMap : register(t1,space3); // the sun shadow map of the dynamic triangles alone
#endif
Texture2D<float3> lightmapAtlases[] : register(t0,space4); // atlas i: full (or rest) at 2i, sun at 2i+1 (R9G9B9E5)
SamplerState lightmapSampler : register(s2); // bilinear, clamped to the atlas, no mips
cbuffer RasterLightmap : register(b2) { // the JSON's calibration; lightmapRest 1: the first map is the rest map
    float lightmapScaleFull; float lightmapScaleSun; float lightmapRest;
};

// The baked light on a static surface. full is all the diffuse light of the bake (lamps, screens, sun, sky, direct and
// bounced), sun its direct sun alone, rest the bake's own denoised full - sun (used when the JSON lists it, so nothing
// is subtracted here). dynShadow (1 where nothing dynamic occludes the sun) masks the baked sun, so characters cast
// their sun shadow on static surfaces; the static shadows are in the bake.
// --ao: not applied here: the bake already holds the static occlusion (SSAO on it darkened contact areas twice).
float3 BakedLighting(float3 albedo, float3 lightmapUvAtlas, float dynShadow)
{
    uint atlas=uint(lightmapUvAtlas.z)*2;
    float3 first=lightmapAtlases[NonUniformResourceIndex(atlas)].SampleLevel(lightmapSampler,lightmapUvAtlas.xy,0);
    float3 sun=lightmapAtlases[NonUniformResourceIndex(atlas+1)].SampleLevel(lightmapSampler,lightmapUvAtlas.xy,0);
    float3 rest=lightmapRest!=0 ? first : max(first-sun,0);
    return albedo*(rest*lightmapScaleFull+sun*lightmapScaleSun*dynShadow);
}
#if PW_LAMPSHADOW
// --lamp-shadows: the bake's third map, the lamp groups' direct light (lamp atlas i at lampAtlasBase + i), is shadowed apart:
// albedo * ((rest + lamp * lampVis) * scaleFull + sun * scaleSun * dynShadow). The loader requires "rest" and "lamp", and
// rest is then full - sun - lamp: indirect light and the sky. lampVis (LampVisibility) darkens only the lamps' direct light,
// never their bounce.
float3 BakedLampLighting(float3 albedo, float3 lightmapUvAtlas, float dynShadow, float lampVis)
{
    uint atlas=uint(lightmapUvAtlas.z);
    float3 rest=lightmapAtlases[NonUniformResourceIndex(atlas*2)].SampleLevel(lightmapSampler,lightmapUvAtlas.xy,0);
    float3 sun=lightmapAtlases[NonUniformResourceIndex(atlas*2+1)].SampleLevel(lightmapSampler,lightmapUvAtlas.xy,0);
    float3 lamp=lightmapAtlases[NonUniformResourceIndex(lampAtlasBase+atlas)].SampleLevel(lightmapSampler,lightmapUvAtlas.xy,0);
    return albedo*((rest+lamp*lampVis)*lightmapScaleFull+sun*lightmapScaleSun*dynShadow);
}
#endif
// The scene pass's light with a lightmap. Static triangles with lightmap UVs (atlas >= 0), as games do it: the baked
// diffuse (dynShadow from the dynamic-only shadow map) plus emission plus the dynamic raster's specular, i.e. the sun's
// and the lamp groups' Bsdf with the diffuse albedo removed. The sun's specular uses sunVisibility, the full shadow map
// of every caster, so static occluders shadow the reflections too. No diffuse sun, lamp or ambient light is added (it
// is in the bake). Metal has albedo 0, so it is emission plus specular, as in RasterShade (whose ambient is albedo times
// --raster-ambient, 0 for metal). Everything else (the dynamic triangles, static ones without lightmap UVs): RasterShade
// with the full shadow map, as without --lightmap.
// --ao: ao reaches only the pixels shaded by RasterShade (characters, static triangles without lightmap UVs); baked
// static surfaces keep their baked occlusion, and the specular added here (glossy, albedo 0) is never occluded.
// --sky-reflections: baked static pixels get the sky's specular too, occluded by the specular occlusion of --ao (the bake
// holds no specular, so nothing counts twice) and the probes' sky visibility.
float3 LightmapShade(Surface s, float3 direction, float sunVisibility, float3 lightmapUvAtlas PW_AO_PARAMETER)
{
    if(lightmapUvAtlas.z<0) return RasterShade(s,direction,sunVisibility PW_AO_ARGUMENT);
#if PW_CSM
    float dynShadow=CsmSunVisibility(dynamicShadowMap,s.position+s.geometric*shadowNormalOffset,CsmViewDepth(s.position));
#else
    float dynShadow=ShadowMapPcf(dynamicShadowMap,s.position+s.geometric*shadowNormalOffset);
#endif
    Surface glossy=s; glossy.albedo=0;
    float3 reflection=0; // the probes' visibility lookup only when the reflections are on
#if PW_AO
    [branch] if(skyReflections!=0) reflection=SkyReflection(s,direction,PW_SKY_VISIBILITY(s),ao);
#else
    [branch] if(skyReflections!=0) reflection=SkyReflection(s,direction,PW_SKY_VISIBILITY(s),1);
#endif
#if PW_LAMPSHADOW
    // The lamps' specular added by RasterDirectLight is shadowed by every caster's maps, as the sun's by the full shadow map.
    return RasterDirectLight(glossy,direction,sunVisibility,BakedLampLighting(s.albedo,lightmapUvAtlas,dynShadow,LampVisibility(s))+s.emission+reflection);
#else
    return RasterDirectLight(glossy,direction,sunVisibility,BakedLighting(s.albedo,lightmapUvAtlas,dynShadow)+s.emission+reflection);
#endif
}
#endif
