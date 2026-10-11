#ifndef PW_RASTER_COMMON_HLSLI
#define PW_RASTER_COMMON_HLSLI
// --render raster: what the scene, shadow and resolve passes share. The path tracer's surface fetch, BSDF and sky
// are reused (bsdf.hlsli -> scene.hlsli); nothing here traces rays.
#include "bsdf.hlsli"
#include "sampling_math.hlsli"
#include "raster_depth.hlsli"
cbuffer RasterFrame : register(b1) {
    row_major float4x4 sunVP; // FitSunShadow (raster_shadow.h)
    float rasterAmbient; uint rasterLightCount; float shadowBias; float shadowNormalOffset;
    float skyReflections; float3 rasterPadding; // 1: --sky-reflections adds the sky's specular (raster_reflections.hlsli)
};
struct RasterLight { float3 position; float padding0; float3 power; float padding1; }; // raster_lights.h
StructuredBuffer<RasterLight> rasterLights : register(t0,space2);
Texture2D<float> shadowMap : register(t1,space2);
SamplerComparisonState shadowSampler : register(s1);
#if PW_SKYAMB
// --raster-sky-ambient (raster_sky.cpp, sky_sh.h): the --hdri sky's irradiance as nine SH coefficients (xyz, w unused), cosine-
// convolved on the CPU. b4 space1 is free in every scene root: raster b0/b1, baked b2 (space0), CSM b2/b3 (space1); the AO scene root adds only t4.
cbuffer RasterSky : register(b4,space1) { float4 skySh[9]; };
// The sky's irradiance at normal n divided by pi: the radiance a white Lambertian surface sends back, what rasterAmbient is
// for the constant term (a constant sky L gives L). The same nine terms and order as SkyShBasis in sky_sh.h.
float3 SkyAmbient(float3 n)
{
    float3 e=skySh[0].xyz*0.282095+(skySh[1].xyz*n.y+skySh[2].xyz*n.z+skySh[3].xyz*n.x)*0.488603
        +(skySh[4].xyz*(n.x*n.y)+skySh[5].xyz*(n.y*n.z)+skySh[7].xyz*(n.x*n.z))*1.092548
        +skySh[6].xyz*(0.315392*(3*n.z*n.z-1))+skySh[8].xyz*(0.546274*(n.x*n.x-n.y*n.y));
    return max(e,0)*(1/PI);
}
#endif
#include "raster_reflections.hlsli"
#if PW_PROBES
// --probes (raster_probes.hlsli): the probes' ambient where the grid has one, else the sky's (with --raster-sky-ambient) or the
// constant. `albedo` already holds --ao's occlusion. `sky` receives the probes' sky visibility (--sky-reflections).
#include "raster_probes.hlsli"
#if PW_SKYAMB
#define PW_AMBIENT(s,albedo,sky) (ProbeAmbient((s),SkyAmbient((s).normal),sky)*(albedo))
#else
#define PW_AMBIENT(s,albedo,sky) (ProbeAmbient((s),rasterAmbient.xxx,sky)*(albedo))
#endif
#define PW_SKY_VISIBILITY(s) ProbeSky(s)
#elif PW_SKYAMB
// RasterShade's ambient term for the surface: the sky's by its normal, or the constant. `albedo` already holds --ao's occlusion.
// Without --probes the sky visibility stays open.
#define PW_AMBIENT(s,albedo,sky) (SkyAmbient((s).normal)*(albedo))
#define PW_SKY_VISIBILITY(s) float4(1,0,0,0)
#else
#define PW_AMBIENT(s,albedo,sky) (rasterAmbient*(albedo))
#define PW_SKY_VISIBILITY(s) float4(1,0,0,0)
#endif
#if PW_AO
// --ao (raster_ao.cpp): the frame's blurred ambient occlusion at the render size, 1 unoccluded. The shading functions
// that take it gain an `ao` parameter in the PW_AO variants alone (PW_AO_PARAMETER, PW_AO_ARGUMENT).
Texture2D<float> aoTexture : register(t4,space2);
#define PW_AO_PARAMETER , float ao
#define PW_AO_ARGUMENT , ao
#else
#define PW_AO_PARAMETER
#define PW_AO_ARGUMENT
#endif
// A lamp group's power is sum(area * emission). The lamps are closed Lambertian surfaces (tubes), so their flux is
// PI * power and an isotropic point light of that flux has the intensity flux / (4 PI) = power / 4.
static const float LampIntensityPerPower=0.25;
// Point lights stand in for lamps a few decimetres long: closer than this, the inverse square is held.
static const float LampMinDistance=0.25;
#if PW_TUBES
#include "raster_tubes.hlsli"
#endif

float3 ShadowCoordinate(float3 position)
{
    float4 clip=mul(sunVP,float4(position,1));
    return float3(clip.xy*float2(0.5,-0.5)+0.5,clip.z-shadowBias);
}
// 1 lit, 0 shadowed: 3x3 comparison samples one texel apart, each a bilinear 2x2 PCF (a 4x4-texel tent).
// Outside the map is lit (border white). `map` is a sun shadow map drawn with sunVP.
float ShadowMapPcf(Texture2D<float> map, float3 position)
{
    float3 s=ShadowCoordinate(position); float sum=0;
    [unroll] for(int y=-1;y<=1;++y) [unroll] for(int x=-1;x<=1;++x) sum+=map.SampleCmpLevelZero(shadowSampler,s.xy,s.z,int2(x,y));
    return sum/9;
}
float SunVisibilityPcf(float3 position) { return ShadowMapPcf(shadowMap,position); }
float LampFalloff(float3 delta) { return LampIntensityPerPower/max(dot(delta,delta),LampMinDistance*LampMinDistance); }
// The sun's visibility at a surface: the one shadow-map lookup of the pixel, used by its sun light and (through the
// colour target's alpha) by its fog. 0 where the visible side faces away from the sun, where Bsdf() is 0 anyway.
float SurfaceSunVisibility(Surface s)
{
    if(sunStrength<=0 || dot(s.geometric,-sunTravel)<=0) return 0;
    return SunVisibilityPcf(s.position+s.geometric*shadowNormalOffset);
}
#if PW_LAMPSHADOW
// --lamp-shadows: each lamp group's spot shadow map (raster_lamp_shadow.hlsli); PW_LAMP_VISIBILITY(i) multiplies light i
// by its group's visibility. Without it the macro is empty and the lamps stay unshadowed.
#include "raster_lamp_shadow.hlsli"
#else
#define PW_LAMP_VISIBILITY(index)
#endif
// `result` plus the light the sun (shadowed by sunVisibility) and every lamp group (unshadowed, or by its spot shadow map with
// --lamp-shadows; with --lamp-tubes its tubes as line lights and its compact emitters as the point light) reflect off the surface
// through Bsdf. With s.albedo 0 that is their specular alone (Bsdf = albedo*(1-F)/PI + specular): --lightmap's static
// surfaces add it to the baked diffuse.
float3 RasterDirectLight(Surface s, float3 direction, float sunVisibility, float3 result)
{
    float3 toSun=-sunTravel;
    float nl=saturate(dot(s.normal,toSun));
    if(nl>0 && sunVisibility>0) result+=Bsdf(s,-direction,toSun)*sunStrength*nl*sunVisibility;
#if PW_LAMPSHADOW
    float lampVisibility[MaxLampShadows]; LampShadowsAt(s,lampVisibility); // every caster's maps
#endif
    for(uint i=0;i<rasterLightCount;++i) {
        float3 delta=rasterLights[i].position-s.position; float length2=dot(delta,delta);
        if(length2<1e-12) continue;
        float3 toLamp=delta*rsqrt(length2); nl=saturate(dot(s.normal,toLamp));
        if(nl>0) result+=Bsdf(s,-direction,toLamp)*rasterLights[i].power*LampFalloff(delta)*nl PW_LAMP_VISIBILITY(i);
    }
#if PW_TUBES
    for(uint k=0;k<rasterLineCount;++k) result+=TubeLight(s,direction,rasterLines[k]) PW_LAMP_VISIBILITY(rasterLightCount+k); // --lamp-tubes: the tubes as line lights
#endif
    return result;
}
// Direct light on a surface: sun with the shadow map, every lamp group unshadowed, emission, the ambient term and (with
// --sky-reflections) the sky's specular.
float3 RasterShade(Surface s, float3 direction, float sunVisibility PW_AO_PARAMETER)
{
    float4 sky=float4(1,0,0,0); // the probes' sky visibility: open without --probes
#if PW_AO
    // --ao occludes all the indirect light, as games apply AO: the ambient term, whatever feeds it (the constant, the sky's
    // SH9, the probes, which hold the sun's and the lamps' bounce light), and the sky's specular through the specular occlusion
    // it derives. Direct light (sun, lamp points, lamp tubes) has its own shadows and stays unoccluded, as does emission.
    float3 ambient=PW_AMBIENT(s,s.albedo*ao,sky);
    return RasterDirectLight(s,direction,sunVisibility,s.emission+ambient+SkyReflection(s,direction,sky,ao));
#else
    float3 ambient=PW_AMBIENT(s,s.albedo,sky);
    return RasterDirectLight(s,direction,sunVisibility,s.emission+ambient+SkyReflection(s,direction,sky,1));
#endif
}
// Analytic distance fog, as games draw it: radiance scattered toward the camera over `distance`, before exposure.
// (1 - T) * (sun * sunVisibility + ambient), with T = MediumTransmittance(--haze, distance). Sun: the white sun's
// strength times the Henyey-Greenstein phase (--haze-g) of the view-to-sun angle. sunVisibility is the shaded
// surface's shadow-map visibility (1 for the sky), the usual cheap game approximation that keeps a shadowed hall from
// filling with sun haze. Ambient: a constant grey of --raster-ambient (an isotropic field; the phase integrates to
// one). No lamps. As the trace's haze (Haze() in haze.hlsli): scattering albedo 0.9, and the sun attenuated from each
// scattering point to the medium's 300-unit boundary, averaged over the ray in 16 steps (HazeSunTransmittance). Until
// 2026-10-07 it was the full 300 units for every point, which left the sky beside and towards the sun at 0.55-0.75 of
// the trace's.
float3 RasterFog(float3 direction, float distance, float sunVisibility)
{
    if(hazeDensity<=0) return 0;
    float sun=sunStrength*PhaseHG(dot(direction,-sunTravel),hazeG)*HazeSunTransmittance(hazeDensity,distance,dot(direction,-sunTravel),300,16);
    return 0.9*(1-MediumTransmittance(hazeDensity,distance))*(sun*sunVisibility+rasterAmbient);
}
#endif
