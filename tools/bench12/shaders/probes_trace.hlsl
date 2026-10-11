// --probes (probes.cpp): traces the irradiance probes once at start. One group of 64 threads per probe, each thread a
// chunk of its rays; the path tracer's own trace, surface fetch, BSDF, sun, lamps and sky (lighting.hlsli), inline
// RayQuery as pathtrace.hlsl. The structure bound at t0 space1 holds the static triangles alone (probes.cpp), so the
// characters receive probe light but do not cast into it.
// What a probe stores is what raster lacks: the sky (or --hdri) on a miss and the light surfaces reflect (their direct
// sun and lamps, shadowed, plus one more bounce). Emission seen directly counts only for emitters without a lamp group:
// the lamp groups are raster's own point lights, and the sun reaches surfaces through raster's shadow map.
#include "lighting.hlsli"
cbuffer ProbePass : register(b1) {
    float3 probeOrigin; float probeSpacing;
    uint3 probeDims; uint probeRays;
    uint probeFirst; float probeInvalidFraction;
};
// Coefficient k of the L1 irradiance (probes.h ProbeSh); alpha: sh0 the validity, sh1..3 the packed free distances along x, y, z.
RWTexture3D<float4> probeSh0 : register(u0);
RWTexture3D<float4> probeSh1 : register(u1);
RWTexture3D<float4> probeSh2 : register(u2);
RWTexture3D<float4> probeSh3 : register(u3);
// The sky visibility (probes.h ProjectProbeSkyVisibility): the fraction of the rays that escape to the sky, in L1 (mean, then 3 x the
// mean of escape x direction), traced against the scene's whole structure: --wind foliage (dynamic) blocks the sky too.
RWTexture3D<float4> probeSky : register(u4);
RaytracingAccelerationStructure fullWorld : register(t2,space1); // Accel::tlas: static, dynamic and instanced
static const uint WindMaterial=8; // TriMaterial flag of the triangles of --wind's veg_ nodes (scene.cpp)
static const uint ProbeThreads=64;
groupshared float3 sumL[ProbeThreads], sumX[ProbeThreads], sumY[ProbeThreads], sumZ[ProbeThreads];
groupshared float4 sumSky[ProbeThreads];
groupshared uint sumBack[ProbeThreads];
groupshared float freeDistance[6]; // +x, -x, +y, -y, +z, -z

// PackProbeFree in probes.h: 32 q(+axis) + q(-axis), q the free distance in 31 steps of the spacing.
float PackFree(float positive, float negative)
{
    return round(saturate(positive/probeSpacing)*31)*32+round(saturate(negative/probeSpacing)*31);
}

// FibonacciDirection in probes.h.
float3 FibonacciDirection(uint i, uint n)
{
    float y=1-2*(float(i)+0.5)/float(n), r=sqrt(max(0,1-y*y)), phi=float(i)*2.39996323;
    return float3(cos(phi)*r,y,sin(phi)*r);
}
// The radiance arriving at the probe from `direction`. back: the ray hit the back of an opaque triangle (its vertex
// normal faces along the ray); alpha-cut cards are two-sided foliage and never count.
float3 ProbeRadiance(float3 origin, float3 direction, inout uint rng, out bool back)
{
    back=false;
    Intersection hit=Trace(origin,direction,0,1e30,false);
    if(!hit.hit) return Sky(direction);
    TriMaterial m=materials[hit.triangleIndex];
    float3 weights=float3(1-hit.bary.x-hit.bary.y,hit.bary);
    float3 normal=WorldVertex(hit.triangleIndex*3,hit.instance).normal*weights.x+WorldVertex(hit.triangleIndex*3+1,hit.instance).normal*weights.y
        +WorldVertex(hit.triangleIndex*3+2,hit.instance).normal*weights.z;
    back=(m.flags&1)==0 && dot(normal,direction)>0;
    Surface s=GetSurface(hit,direction);
    float3 result=m.lampGroup==0xffffffffu ? s.emission : 0;
    result+=Direct(s,-direction,rng,true);
    // One bounce, sampled and weighted as the path tracer's Radiance(): BSDF sampling, the sky on a miss, the next
    // surface's direct light, and its emission with the complementary lamp MIS weight.
    bool specular;
    float3 bounce=SampleBsdf(s,-direction,rng,specular);
    float pdf=BsdfPdf(s,-direction,bounce);
    if(pdf<=0) return result;
    float3 throughput=Bsdf(s,-direction,bounce)*saturate(dot(s.normal,bounce))/pdf;
    Intersection next=Trace(s.position+s.geometric*0.001,bounce,0.001,1e30,false);
    if(!next.hit) return result+throughput*Sky(bounce);
    Surface t=GetSurface(next,bounce);
    TriMaterial lamp=materials[next.triangleIndex];
    float3 a=WorldPosition(next.triangleIndex*3,next.instance);
    float3 lightNormal=normalize(cross(WorldPosition(next.triangleIndex*3+1,next.instance)-a,WorldPosition(next.triangleIndex*3+2,next.instance)-a));
    float distance=length(t.position-s.position);
    float lampPdf=lightPower>0 && lamp.lampGroup!=0xffffffffu ? Luminance(Emission(lamp))/lightPower*distance*distance/max(abs(dot(lightNormal,-bounce)),1e-8) : 0;
    return result+throughput*(t.emission*(1-LampMis(lampPdf,pdf))+Direct(t,-bounce,rng,false));
}
// 1 when nothing between `origin` and the sky stops a ray along `direction`: static and instanced triangles and --wind's foliage
// (dynamic, at its start pose; it sways but stays). Other dynamic triangles (the characters) are not where they walk later, so
// they never block it, as they cast nothing into the probes' light.
float Escapes(float3 origin, float3 direction)
{
    RayDesc ray; ray.Origin=origin; ray.Direction=direction; ray.TMin=0; ray.TMax=1e30;
    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES|RAY_FLAG_FORCE_NON_OPAQUE|RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> query;
    query.TraceRayInline(fullWorld,RAY_FLAG_NONE,255,ray);
    while(query.Proceed()) {
        uint id=query.CandidateInstanceID();
        uint triangleIndex=TriangleIndex(id,query.CandidateGeometryIndex(),query.CandidatePrimitiveIndex());
        if(id==1 && (materials[triangleIndex].flags&WindMaterial)==0) continue;
        if(Accept(triangleIndex,query.CandidateTriangleBarycentrics(),SceneInstanceOf(id))) query.CommitNonOpaqueTriangleHit();
    }
    return query.CommittedStatus()==COMMITTED_TRIANGLE_HIT ? 0 : 1;
}
[numthreads(ProbeThreads,1,1)]
void CS(uint3 group : SV_GroupID, uint thread : SV_GroupIndex)
{
    uint probe=probeFirst+group.x;
    uint3 cell=uint3(probe%probeDims.x,(probe/probeDims.x)%probeDims.y,probe/(probeDims.x*probeDims.y));
    float3 origin=probeOrigin+float3(cell)*probeSpacing;
    float3 l=0, x=0, y=0, z=0; float4 sky=0; uint back=0;
    if(thread<6) { // the free distance along one axis direction, up to one spacing (the lookup's visibility test)
        float3 axis=0; axis[thread/2]=(thread&1) ? -1 : 1;
        Intersection hit=Trace(origin,axis,0,probeSpacing,false);
        freeDistance[thread]=hit.hit ? hit.distance : probeSpacing;
    }
    for(uint i=thread;i<probeRays;i+=ProbeThreads) {
        float3 direction=FibonacciDirection(i,probeRays);
        uint rng=Hash(probe*0x9e3779b9u+i)^Hash(i+0x85ebca6bu);
        bool backFace;
        float3 radiance=max(ProbeRadiance(origin,direction,rng,backFace),0);
        if(!all(isfinite(radiance))) radiance=0;
        l+=radiance; x+=radiance*direction.x; y+=radiance*direction.y; z+=radiance*direction.z; back+=backFace ? 1 : 0;
        sky+=Escapes(origin,direction)*float4(1,direction);
    }
    sumL[thread]=l; sumX[thread]=x; sumY[thread]=y; sumZ[thread]=z; sumSky[thread]=sky; sumBack[thread]=back;
    GroupMemoryBarrierWithGroupSync();
    [unroll] for(uint stride=ProbeThreads/2;stride>0;stride>>=1) {
        if(thread<stride) {
            sumL[thread]+=sumL[thread+stride]; sumX[thread]+=sumX[thread+stride]; sumY[thread]+=sumY[thread+stride];
            sumZ[thread]+=sumZ[thread+stride]; sumSky[thread]+=sumSky[thread+stride]; sumBack[thread]+=sumBack[thread+stride];
        }
        GroupMemoryBarrierWithGroupSync();
    }
    if(thread!=0 || probe>=probeDims.x*probeDims.y*probeDims.z) return;
    // ProjectProbeSh in probes.h: pi * mean(L), 2 pi * mean(L * w).
    float inverse=1.0/float(probeRays), valid=float(sumBack[0])*inverse>probeInvalidFraction ? 0 : 1;
    probeSh0[cell]=float4(sumL[0]*(PI*inverse),valid);
    probeSh1[cell]=float4(sumX[0]*(2*PI*inverse),PackFree(freeDistance[0],freeDistance[1]));
    probeSh2[cell]=float4(sumY[0]*(2*PI*inverse),PackFree(freeDistance[2],freeDistance[3]));
    probeSh3[cell]=float4(sumZ[0]*(2*PI*inverse),PackFree(freeDistance[4],freeDistance[5]));
    probeSky[cell]=sumSky[0]*float4(inverse,3*inverse.xxx);
}
