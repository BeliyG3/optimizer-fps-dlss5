// --render raster, scene pass: vertex pulling from the scene's triangle list, the path tracer's surface fetch
// (materials, textures, normal maps, alpha cutoff) and raster lighting. Writes the lit colour (exposure applied,
// before fog) and the motion guide; the resolve adds fog and the sky.
// Compiled with PW_LIGHTMAP=1 for --lightmap (raster_lightmap_*.cso): static triangles then take the baked light.
// Compiled with PW_CSM=1 for --csm (raster_csm_ps.cso, raster_csm_lightmap_ps.cso): the sun shadow in cascades.
// Compiled with PW_AO=1 for --ao (raster_ao*_ps.cso, alone or with the above): ambient occlusion from the prepass
// (raster_prepass.hlsl, raster_ao.hlsl) on the ambient and lamp diffuse light; the depth test is EQUAL to the prepass.
#if PW_LIGHTMAP
#include "raster_lightmap.hlsli"
#else
#include "raster_common.hlsli"
#endif
#if PW_CSM
#include "raster_csm.hlsli"
#endif
struct RasterVertex {
    float4 position : SV_Position;
    float2 bary : BARYCENTRIC; // weights of the triangle's second and third vertex, as Intersection.bary
    nointerpolation uint triangleIndex : TRIANGLE;
    nointerpolation uint instance : INSTANCEINDEX; // NoInstance for the flattened triangles
#if PW_LIGHTMAP
    float2 lightmapUv : LIGHTMAPUV;
    nointerpolation float lightmapAtlas : LIGHTMAPATLAS; // -1: no baked light (dynamic, or no TEXCOORD_1)
#endif
};
// `instance`: the per-instance vertex buffer's entry (raster_instances.h): NoInstance for the flattened range, else the
// SceneInstance whose transform places this draw's object-space mesh.
RasterVertex VS(uint id : SV_VertexID, uint instance : INSTANCE)
{
    id=StoredVertex(id,instance);
    RasterVertex o; uint corner=id%3;
    float4 clip=mul(currentVP,float4(WorldPosition(id,instance),1));
    // The trace's jitter convention: a positive jitter moves the image right/down by that many render pixels.
    clip.xy+=float2(2*jitterX/width,-2*jitterY/height)*clip.w;
    o.position=clip; o.bary=float2(corner==1 ? 1 : 0,corner==2 ? 1 : 0); o.triangleIndex=id/3; o.instance=instance;
#if PW_LIGHTMAP
    o.lightmapUv=lightmapUvs[id].uv; o.lightmapAtlas=lightmapUvs[id].atlas;
#endif
    return o;
}
float2 Pixel(float4 clip) { return (clip.xy/clip.w*float2(0.5,-0.5)+0.5)*float2(width,height); }
struct RasterTargets { float4 colour : SV_Target0; float2 motion : SV_Target1; };
RasterTargets PS(RasterVertex i)
{
    if(!Accept(i.triangleIndex,i.bary,i.instance)) discard;
    Intersection hit=(Intersection)0; hit.hit=true; hit.triangleIndex=i.triangleIndex; hit.bary=i.bary; hit.instance=i.instance;
    float3 direction=normalize(TrianglePosition(i.triangleIndex,i.bary,i.instance)-eye);
    Surface s=GetSurface(hit,direction);
    RasterTargets o;
    // Alpha carries the sun visibility to the resolve's fog (the target is RGBA16F; nothing else reads its alpha).
#if PW_CSM
    float sunVisibility=CsmSurfaceSunVisibility(s);
#else
    float sunVisibility=SurfaceSunVisibility(s);
#endif
#if PW_AO
    float ao=aoTexture.Load(int3(i.position.xy,0));
#endif
#if PW_LIGHTMAP
    // The fog keeps the full shadow map's visibility on baked surfaces too.
    o.colour=float4(max(LightmapShade(s,direction,sunVisibility,float3(i.lightmapUv,i.lightmapAtlas) PW_AO_ARGUMENT)*exposure,0),sunVisibility);
#else
    o.colour=float4(max(RasterShade(s,direction,sunVisibility PW_AO_ARGUMENT)*exposure,0),sunVisibility);
#endif
    if(!all(isfinite(o.colour.rgb))) o.colour.rgb=0;
    // The trace's motion: unjittered matrices, current-to-previous pixels (or the NDC layout).
    float4 now=mul(currentVP,float4(s.position,1)), old=mul(previousVP,float4(s.previous,1));
    float2 delta=old.w>1e-5 ? Pixel(old)-Pixel(now) : float2(0,0);
    o.motion=motionNdc!=0 ? delta/float2(-0.5*width,0.5*height) : delta;
    if(viewMode==8u) o.motion=float2(float((materials[i.triangleIndex].flags>>16)&7u),0); // --view vegmask: the foliage class
    if(viewMode==9u) o.motion=float2(old.w,0); // --dump-raw prevdepth: this point's view depth in the span-back camera
    return o;
}
