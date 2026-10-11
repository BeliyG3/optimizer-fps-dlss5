#ifndef PW_SKY_REFLECTION_MATH_HLSLI
#define PW_SKY_REFLECTION_MATH_HLSLI
// --sky-reflections (RASTER_REFLECTIONS.md): the image-based sky specular's math, shared by the prefilter passes
// (sky_prefilter.hlsl), the scene pass (raster_reflections.hlsli), the DFG table built on the CPU (sky_reflections.h)
// and the checks (tests_reflections.cpp, through hlsl_compat.h). Roughness is the bench's perceptual roughness:
// GGX a2 = roughness^4, as Bsdf() in bsdf.hlsli; the specular lobe is Bsdf's (GGX D, the separable Smith G1 of both
// directions, Schlick's Fresnel of the view and half vectors).

static const uint SkySourceSize=256;    // the sky as a cube, every mip down to 1 texel: the prefilter's source
static const uint SkySourceMips=9;
static const uint SkyReflectionSize=128; // the prefiltered cube's top mip (UE's default sky light cubemap size)
static const uint SkyReflectionMips=6;   // mip m holds roughness m / 5: 0, 0.2, ..., 1
static const uint SkyPrefilterSamples=512;
static const uint SkyDfgSize=32;         // the DFG table: n.v across, roughness down, texel centres
static const uint SkyDfgSamples=4096;
static const float SkyPi=3.14159265f;

// The direction through (u, v), each -1..1 across a face, of cube face `face` in D3D's TextureCube layout
// (+x, -x, +y, -y, +z, -z; v grows downwards), normalised.
float3 CubeFaceDirection(uint face, float u, float v)
{
    float3 d=face==0 ? float3(1,-v,-u) : face==1 ? float3(-1,-v,u) : face==2 ? float3(u,1,v)
        : face==3 ? float3(u,-1,-v) : face==4 ? float3(u,-v,1) : float3(-u,-v,-1);
    return normalize(d);
}
// The prefiltered cube's mip for a roughness (linear: mip m was filtered at roughness m / (mips - 1)), and back.
float SkyReflectionLod(float roughness) { return saturate(roughness)*float(SkyReflectionMips-1); }
float SkyMipRoughness(uint mip) { return float(mip)/float(SkyReflectionMips-1); }

// Hammersley point i of n: (i + 0.5) / n and the radical inverse of i.
float RadicalInverse(uint bits)
{
    bits=(bits<<16u)|(bits>>16u);
    bits=((bits&0x55555555u)<<1u)|((bits&0xAAAAAAAAu)>>1u);
    bits=((bits&0x33333333u)<<2u)|((bits&0xCCCCCCCCu)>>2u);
    bits=((bits&0x0F0F0F0Fu)<<4u)|((bits&0xF0F0F0F0u)>>4u);
    bits=((bits&0x00FF00FFu)<<8u)|((bits&0xFF00FF00u)>>8u);
    return float(bits)*2.3283064365386963e-10f;
}
float2 Hammersley(uint i, uint n) { return float2((float(i)+0.5f)/float(n),RadicalInverse(i)); }
// A GGX half vector around +z for a2 = roughness^4, as SampleBsdf draws it.
float3 GgxHalfVector(float2 xi, float a2)
{
    float c=sqrt((1-xi.x)/(1+(a2-1)*xi.x)), s=sqrt(max(0.0f,1-c*c)), phi=2*SkyPi*xi.y;
    return float3(s*cos(phi),s*sin(phi),c);
}
float GgxDistribution(float nh, float a2) { float d=nh*nh*(a2-1)+1; return a2/(SkyPi*d*d); }
float SmithG1(float c, float a2) { return 2*c/max(c+sqrt(a2+(1-a2)*c*c),1e-7f); }
// `local` (z along n) in world space, with BasisSample's tangent frame.
float3 AroundNormal(float3 local, float3 n)
{
    float3 t=normalize(cross(abs(n.y)<0.99f ? float3(0,1,0) : float3(1,0,0),n));
    return local.x*t+local.y*cross(n,t)+local.z*n;
}

// The split sum's second factor for a view at n.v `nv`: the specular reflectance of a white sky is f0 * A + B, the
// integral of Bsdf's specular lobe times n.l over the hemisphere (Schlick's Fresnel split into f0 and 1 - f0 parts).
// GGX importance sampling (Karis): each sample weighs G1(l) G1(v) v.h / (n.h n.v).
float2 IntegrateDfg(float nv, float roughness, uint samples)
{
    float a2=pow(roughness,4.0f); nv=max(nv,1e-4f);
    float3 v=float3(sqrt(max(0.0f,1-nv*nv)),0,nv);
    float a=0, b=0;
    for(uint i=0;i<samples;++i) {
        float3 h=GgxHalfVector(Hammersley(i,samples),a2);
        float vh=dot(v,h); float3 l=h*(2*vh)-v;
        if(l.z<=0 || vh<=0) continue;
        float g=SmithG1(l.z,a2)*SmithG1(nv,a2)*vh/max(h.z*nv,1e-7f), fc=pow(1-vh,5.0f);
        a+=(1-fc)*g; b+=fc*g;
    }
    return float2(a,b)/float(samples);
}

// Specular occlusion from ambient occlusion (Lagarde and de Rousiers, "Moving Frostbite to PBR", 2014): a rough lobe is
// occluded about as much as the diffuse light, a smooth one seen head-on hardly at all. alpha = roughness^2.
float SpecularOcclusion(float nv, float ao, float roughness)
{
    float alpha=roughness*roughness;
    return saturate(pow(max(nv+ao,0.0f),exp2(-16*alpha-1))-1+ao);
}

// The probes' L1 sky visibility as the reflection uses it: L1 cannot hold a closed room with a few openings, it rings to about
// 0.1-0.25 in every direction inside it (the hall), and the sky is 10-50 times brighter than such a room. The visibility is
// rescaled from SkyVisibilityFloor (0) to 1 (1), as UE's sky light occlusion contrast: the ringing goes, open sky stays open.
static const float SkyVisibilityFloor=0.15f;
float ReflectionVisibility(float v) { return saturate((v-SkyVisibilityFloor)/(1-SkyVisibilityFloor)); }

#ifdef PW_PREFILTER_SOURCE
// The prefiltered radiance around n at `roughness`, N = V = R (Karis' split sum): GGX samples of the reflected direction
// weighted by n.l, each read from the source cube at the mip whose texel matches the sample's solid angle (filtered
// importance sampling, + 1 mip). PW_PREFILTER_SOURCE(direction, lod) is the includer's source lookup.
float3 PrefilterGgx(float3 n, float roughness, uint samples)
{
    float a2=pow(roughness,4.0f), texel=4*SkyPi/(6.0f*float(SkySourceSize)*float(SkySourceSize));
    float3 sum=float3(0,0,0); float weight=0;
    for(uint i=0;i<samples;++i) {
        float3 h=AroundNormal(GgxHalfVector(Hammersley(i,samples),a2),n);
        float nh=dot(n,h); float3 l=h*(2*nh)-n; float nl=dot(n,l);
        if(nl<=0) continue;
        float pdf=GgxDistribution(nh,a2)*0.25f; // D n.h / (4 v.h), v.h = n.h
        float lod=max(0.5f*log2(1/(float(samples)*max(pdf,1e-8f)*texel))+1,0.0f);
        sum=sum+PW_PREFILTER_SOURCE(l,lod)*nl; weight+=nl;
    }
    return weight>0 ? sum*(1/weight) : PW_PREFILTER_SOURCE(n,0.0f);
}
#endif
#endif
