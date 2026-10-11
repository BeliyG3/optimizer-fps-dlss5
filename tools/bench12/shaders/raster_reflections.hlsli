#ifndef PW_RASTER_REFLECTIONS_HLSLI
#define PW_RASTER_REFLECTIONS_HLSLI
// --sky-reflections (raster_reflections.cpp, RASTER_REFLECTIONS.md): the sky's image-based specular in the scene pass, as
// games add their sky light's reflection: the prefiltered cube in the reflected direction at the surface's roughness times
// the split sum's DFG of its f0, occluded by the specular occlusion of --ao and by the probes' sky visibility.
// Included by raster_common.hlsli (Surface comes from scene.hlsli). Space 8 is used by no other root: one table (the cube,
// the DFG table) appended to every scene root; s3 is a trilinear clamped sampler of every scene root.
#include "sky_reflection_math.hlsli"
TextureCube<float4> skyReflection : register(t0,space8);
Texture2D<float2> skyDfg : register(t1,space8);
SamplerState skyReflectionSampler : register(s3);

// The probes' sky visibility in direction d: L1 of the fraction of rays that escape to the sky (probes.h
// EvalProbeSkyVisibility), rescaled past its ringing (ReflectionVisibility). (1, 0, 0, 0) without --probes or outside the
// grid: open everywhere.
float SkyVisibility(float4 v, float3 d) { return ReflectionVisibility(saturate(v.x+dot(v.yzw,d))); }
// The sky's specular off surface s seen along `direction` (before exposure; 0 unless --sky-reflections is on). `sky`: the
// probes' visibility coefficients; `ao`: --ao's occlusion of the pixel (1 without it).
float3 SkyReflection(Surface s, float3 direction, float4 sky, float ao)
{
    [branch] if(skyReflections==0) return 0;
    float nv=saturate(dot(s.normal,-direction));
    float3 r=reflect(direction,s.normal);
    float3 radiance=skyReflection.SampleLevel(skyReflectionSampler,r,SkyReflectionLod(s.roughness)).rgb;
    float2 dfg=skyDfg.SampleLevel(skyReflectionSampler,float2(nv,s.roughness),0);
    return radiance*(s.f0*dfg.x+dfg.y)*(SpecularOcclusion(nv,ao,s.roughness)*SkyVisibility(sky,r));
}
#endif
