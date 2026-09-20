#ifndef PW_SCENE_HLSLI
#define PW_SCENE_HLSLI
#include "frame.hlsli"
static const float PI=3.14159265359;
struct TriVertex { float3 pos, prevPos, normal; float2 uv; };
struct TriMaterial { float3 albedo; float roughness; float3 emission; int textureIndex; float alphaCutoff; uint flags;
    int metallicRoughnessTexture, normalTexture; float metallic, normalScale; uint lampGroup; };
struct EmissiveTriangle { uint triangleIndex; float area, cdf, power; };
StructuredBuffer<TriVertex> vertices : register(t0);
StructuredBuffer<TriMaterial> materials : register(t1);
StructuredBuffer<EmissiveTriangle> lights : register(t2);
Texture2D<float4> environment : register(t3);
Texture2D<float4> textures[] : register(t4);
StructuredBuffer<float4> lampGroups : register(t1,space1);
float3 Emission(TriMaterial m)
{
    // Root SRVs have no bounds metadata; never speculatively index the sentinel.
    [branch] if(m.lampGroup==0xffffffffu) return m.emission;
    return m.emission*lampGroups[m.lampGroup].rgb;
}
RaytracingAccelerationStructure world : register(t0,space1);
SamplerState linearWrap : register(s0);

uint Hash(uint v)
{
    uint state=v*747796405u+2891336453u;
    uint word=((state>>((state>>28u)+4u))^state)*277803737u;
    return (word>>22u)^word;
}
float Random(inout uint state) { state=Hash(state); return (state>>8)* (1.0/16777216.0); }
float3 BasisSample(float3 local, float3 axis)
{
    float3 tangent=normalize(cross(abs(axis.y)<0.99 ? float3(0,1,0) : float3(1,0,0),axis));
    return local.x*tangent+local.y*cross(axis,tangent)+local.z*axis;
}
float2 TriangleUv(uint triangleIndex, float2 bary)
{
    return vertices[triangleIndex*3].uv*(1-bary.x-bary.y)+vertices[triangleIndex*3+1].uv*bary.x+vertices[triangleIndex*3+2].uv*bary.y;
}
float TextureLod(float distance) { return max(0,log2(max(distance*tanHalf*540.0/height,1))); }
float4 MaterialTexture(int index, float2 uv, float distance)
{
    if(index<0) return 1;
    return textures[NonUniformResourceIndex(index)].SampleLevel(linearWrap,uv,TextureLod(distance));
}
float4 BaseTexture(TriMaterial m, float2 uv, float distance)
{
    return MaterialTexture(m.textureIndex,uv,distance);
}
bool Accept(uint triangleIndex, float2 bary)
{
    TriMaterial m=materials[triangleIndex];
    float3 position=vertices[triangleIndex*3].pos*(1-bary.x-bary.y)+vertices[triangleIndex*3+1].pos*bary.x+vertices[triangleIndex*3+2].pos*bary.y;
    return (m.flags&1)==0 || BaseTexture(m,TriangleUv(triangleIndex,bary),length(position-eye)).a>=m.alphaCutoff;
}
struct Intersection { bool hit; uint triangleIndex; float2 bary; float distance; };
uint TriangleIndex(uint instance, uint geometry, uint primitive)
{
    uint base=instance==1 ? dynamicBase : 0;
    uint opaque=instance==1 ? dynamicOpaque : opaqueTriangles;
    return base+primitive+(geometry>0 ? opaque : 0);
}
Intersection Trace(float3 origin, float3 direction, float minimum, float maximum, bool shadow)
{
    RayDesc ray; ray.Origin=origin; ray.Direction=direction; ray.TMin=minimum; ray.TMax=maximum;
    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> query;
    query.TraceRayInline(world,shadow ? RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH : RAY_FLAG_NONE,255,ray);
    while(query.Proceed()) {
        if(query.CandidateType()==CANDIDATE_NON_OPAQUE_TRIANGLE) {
            uint triangleIndex=TriangleIndex(query.CandidateInstanceID(),query.CandidateGeometryIndex(),query.CandidatePrimitiveIndex());
            if(Accept(triangleIndex,query.CandidateTriangleBarycentrics())) query.CommitNonOpaqueTriangleHit();
        }
    }
    Intersection result=(Intersection)0;
    result.hit=query.CommittedStatus()==COMMITTED_TRIANGLE_HIT;
    if(result.hit) {
        result.triangleIndex=TriangleIndex(query.CommittedInstanceID(),query.CommittedGeometryIndex(),query.CommittedPrimitiveIndex());
        result.bary=query.CommittedTriangleBarycentrics(); result.distance=query.CommittedRayT();
    }
    return result;
}
struct Surface {
    float3 position, previous, normal, geometric, albedo, emission, f0;
    float roughness; uint flags;
};
Surface GetSurface(Intersection hit, float3 incoming)
{
    TriVertex a=vertices[hit.triangleIndex*3], b=vertices[hit.triangleIndex*3+1], c=vertices[hit.triangleIndex*3+2];
    float3 weights=float3(1-hit.bary.x-hit.bary.y,hit.bary);
    TriMaterial m=materials[hit.triangleIndex]; Surface s;
    s.position=a.pos*weights.x+b.pos*weights.y+c.pos*weights.z;
    s.previous=a.prevPos*weights.x+b.prevPos*weights.y+c.prevPos*weights.z;
    s.geometric=normalize(cross(b.pos-a.pos,c.pos-a.pos));
    if(dot(s.geometric,incoming)>0) s.geometric=-s.geometric;
    float3 n=a.normal*weights.x+b.normal*weights.y+c.normal*weights.z;
    s.normal=dot(n,n)>1e-12 ? normalize(n) : s.geometric;
    if(dot(s.normal,s.geometric)<0) s.normal=-s.normal;
    if(dot(s.normal,incoming)>=0) s.normal=s.geometric;
    float2 uv=TriangleUv(hit.triangleIndex,hit.bary); float distance=length(s.position-eye);
    float4 tex=BaseTexture(m,uv,distance);
    float4 mr=MaterialTexture(m.metallicRoughnessTexture,uv,distance);
    float metal=saturate(m.metallic*mr.b);
    float3 base=saturate(m.albedo*tex.rgb*albedoScale);
    s.f0=lerp(0.04.xxx,base,metal); s.albedo=base*(1-metal);
    s.emission=Emission(m)*tex.rgb;
    s.roughness=clamp(m.roughness*mr.g,0.04,1); s.flags=m.flags;
    float2 uv1=b.uv-a.uv, uv2=c.uv-a.uv;
    float determinant=uv1.x*uv2.y-uv1.y*uv2.x;
    if(m.normalTexture>=0 && abs(determinant)>1e-10) {
        float3 t=((b.pos-a.pos)*uv2.y-(c.pos-a.pos)*uv1.y)/determinant;
        float3 bitangent=((c.pos-a.pos)*uv1.x-(b.pos-a.pos)*uv2.x)/determinant;
        t-=s.normal*dot(s.normal,t);
        if(dot(t,t)>1e-12) {
            t=normalize(t);
            float3 bframe=cross(s.normal,t)*(dot(cross(s.normal,t),bitangent)<0 ? -1 : 1);
            float3 mapped=MaterialTexture(m.normalTexture,uv,distance).xyz*2-1;
            mapped.xy*=m.normalScale;
            float3 nmap=t*mapped.x+bframe*mapped.y+s.normal*mapped.z;
            if(dot(nmap,nmap)>1e-12) {
                nmap=normalize(nmap);
                if(dot(nmap,s.geometric)>0 && dot(nmap,incoming)<0) s.normal=nmap;
            }
        }
    }
    return s;
}
float3 Sky(float3 direction)
{
    float2 uv=float2(atan2(direction.z,direction.x)/(2*PI)+0.5,acos(clamp(direction.y,-1,1))/PI);
    // Clamp latitude to texel centres; longitude alone is periodic.
    uint w,h; environment.GetDimensions(w,h); uv.y=clamp(uv.y,0.5/h,1-0.5/h);
    return environment.SampleLevel(linearWrap,uv,max(0,log2(max(float(w)/width,1)))).rgb;
}
float Luminance(float3 v) { return dot(v,float3(0.2126,0.7152,0.0722)); }
#endif
