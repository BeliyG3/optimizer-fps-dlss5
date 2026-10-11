#ifndef PW_RASTER_PROBES_HLSLI
#define PW_RASTER_PROBES_HLSLI
// --probes (probes.h, raster_probes.cpp): the ambient term from the irradiance probes, in the PW_PROBES scene shaders.
// Included by raster_common.hlsli (Surface and PI come from scene.hlsli). Space 5 is free in every scene root: the raster
// root uses space0 (b0, b1, t0..t3 and the textures from t4), t1 space1 and space2 (lights, shadow map, cascades t3, --ao
// t4); --lightmap adds space3, space4 and b2; --csm b2 and b3 space1; --raster-sky-ambient b4 space1. One descriptor
// table (raster_pipeline.cpp ProbeTable) holds the constants and the five textures.
cbuffer RasterProbes : register(b0,space5) { // probes.cpp ProbeConstants
    float3 probeOrigin; float probeSpacing;
    uint3 probeDims; float probeNormalBias;
    float probeSideBand; float3 probePadding;
};
// Coefficient k of the L1 irradiance of every probe (E(n) = c0 + c1 n.x + c2 n.y + c3 n.z); alpha: probeSh0 the validity,
// probeSh1..3 the packed free distances along x, y, z.
Texture3D<float4> probeSh0 : register(t0,space5);
Texture3D<float4> probeSh1 : register(t1,space5);
Texture3D<float4> probeSh2 : register(t2,space5);
Texture3D<float4> probeSh3 : register(t3,space5);
// The sky visibility of every probe (probes.h ProjectProbeSkyVisibility): L1 of the fraction of its rays that escape to the sky,
// read by --sky-reflections (SkyVisibility in raster_reflections.hlsli).
Texture3D<float4> probeSky : register(t4,space5);

// ProbeSide in probes.h: 1 in front of the surface, 0 from probeSideBand behind it.
float ProbeSide(float distance) { return saturate(1+distance/probeSideBand); }
// ProbeVisible in probes.h: 0 when the lookup point lies beyond the probe's free distance along an axis (packed: the alphas
// of sh1..3, 32 q(+axis) + q(-axis), q in 31 steps of the spacing).
float ProbeVisible(float3 offset, float3 packed)
{
    float3 high=floor(packed/32), low=packed-high*32;
    float3 free=lerp(low,high,float3(offset>=0))*(probeSpacing/31);
    return all(abs(offset)<=free+0.05*probeSpacing) ? 1 : 0;
}
// The probes' ambient for the surface (irradiance / pi, as SkyAmbient): the eight corners around the point moved
// probeNormalBias along the normal, weighted trilinear x validity x side x visibility and renormalised (ProbeWeights in
// probes.h; without a valid corner in front and in sight, trilinear x validity). `fallback` outside the grid or without a
// valid corner. `sky`: the probes' sky visibility with the same weights; open (1, 0, 0, 0) where the ambient falls back.
float3 ProbeAmbient(Surface s, float3 fallback, out float4 sky)
{
    sky=float4(1,0,0,0);
    float3 last=float3(probeDims)-1;
    float3 lookup=s.position+s.normal*probeNormalBias, p=(lookup-probeOrigin)/probeSpacing;
    if(any(p<-0.5) || any(p>last+0.5)) return fallback;
    p=clamp(p,0,last);
    uint3 base=min(uint3(p),max(probeDims,2)-2);
    float3 f=saturate(p-float3(base));
    float4 front[4]={0.xxxx,0.xxxx,0.xxxx,0.xxxx}, valid[4]={0.xxxx,0.xxxx,0.xxxx,0.xxxx}, frontSky=0, validSky=0;
    float frontSum=0, validSum=0;
    [unroll] for(uint k=0;k<8;++k) {
        uint3 offset=uint3(k&1,(k>>1)&1,k>>2);
        int4 cell=int4(min(base+offset,probeDims-1),0);
        float3 t=lerp(1-f,f,float3(offset));
        float4 c0=probeSh0.Load(cell);
        float w=t.x*t.y*t.z*c0.a;
        if(w<=0) continue;
        float4 c[4]={c0,probeSh1.Load(cell),probeSh2.Load(cell),probeSh3.Load(cell)};
        float3 probe=probeOrigin+float3(cell.xyz)*probeSpacing;
        float side=w*ProbeSide(dot(probe-s.position,s.normal))*ProbeVisible(lookup-probe,float3(c[1].a,c[2].a,c[3].a));
        [unroll] for(uint j=0;j<4;++j) { valid[j]+=w*c[j]; front[j]+=side*c[j]; }
        float4 v=probeSky.Load(cell); validSky+=w*v; frontSky+=side*v;
        validSum+=w; frontSum+=side;
    }
    if(validSum<=1e-6) return fallback;
    float4 sh[4]; float sum=frontSum>1e-6 ? frontSum : validSum;
    [unroll] for(uint j=0;j<4;++j) sh[j]=(frontSum>1e-6 ? front[j] : valid[j])/sum;
    sky=(frontSum>1e-6 ? frontSky : validSky)/sum;
    float3 e=sh[0].rgb+sh[1].rgb*s.normal.x+sh[2].rgb*s.normal.y+sh[3].rgb*s.normal.z;
    return max(e,0)*(1/PI);
}
// The probes' sky visibility alone (--lightmap's baked static pixels, which take no probe ambient).
float4 ProbeSky(Surface s) { float4 sky; ProbeAmbient(s,0,sky); return sky; }
#endif
