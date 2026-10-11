// --render raster --ao, depth/normal prepass: the scene pass's own vertex shader (raster_vs.cso, or
// raster_lightmap_vs.cso with --lightmap, which this file's PW_LIGHTMAP variant matches), so the scene pass after it can
// test its depth EQUAL. Writes the depth and the octahedral geometric normal (RG16_SNORM), with the alpha cutoff.
// The geometric normal is the plane the depth buffer holds, so a flat surface never occludes itself; a normal-mapped
// or interpolated normal tilts away from that plane and speckles the AO.
// PrepassMotion (--ao game, raster_prepass_motion*_ps.cso) also writes the motion its temporal filter reprojects with.
#include "raster.hlsl"
#include "raster_ao_math.hlsli"
float2 Prepass(RasterVertex i) : SV_Target0
{
    if(!Accept(i.triangleIndex,i.bary,i.instance)) discard;
    uint first=i.triangleIndex*3;
    float3 a=WorldPosition(first,i.instance), b=WorldPosition(first+1,i.instance), c=WorldPosition(first+2,i.instance);
    float3 direction=TrianglePosition(i.triangleIndex,i.bary,i.instance)-eye;
    // As GetSurface's geometric normal: the triangle's, turned toward the camera.
    float3 normal=normalize(cross(b-a,c-a));
    return OctEncode(dot(normal,direction)>0 ? -normal : normal);
}
// --ao game: the prepass's normal plus the motion (RG16F), raster.hlsl's formula (unjittered matrices, current-to-previous
// pixels) but always in pixels, whatever --motion says: the AO's temporal filter runs before the scene pass writes its
// own motion, and --motion none zeroes only that one.
struct PrepassTargets { float2 normal : SV_Target0; float2 motion : SV_Target1; };
PrepassTargets PrepassMotion(RasterVertex i)
{
    PrepassTargets o; o.normal=Prepass(i);
    uint first=i.triangleIndex*3; float3 weights=float3(1-i.bary.x-i.bary.y,i.bary);
    float3 previous=WorldVertex(first,i.instance).prevPos*weights.x+WorldVertex(first+1,i.instance).prevPos*weights.y
        +WorldVertex(first+2,i.instance).prevPos*weights.z;
    float4 now=mul(currentVP,float4(TrianglePosition(i.triangleIndex,i.bary,i.instance),1)), old=mul(previousVP,float4(previous,1));
    o.motion=old.w>1e-5 ? Pixel(old)-Pixel(now) : float2(0,0);
    return o;
}
// --lamp-tubes without --ao (raster_tubes_prepass*_ps.cso): depth alone, with the alpha cutoff, so the scene pass after it
// tests EQUAL and evaluates its line lights once per pixel instead of once per drawn fragment.
void PrepassDepth(RasterVertex i)
{
    if(!Accept(i.triangleIndex,i.bary,i.instance)) discard;
}
// --cull-debug (raster_cull_debug_ps.cso): the small instances the Hi-Z culled, drawn over the finished scene pass without a
// depth test; the blend multiplies the colour below by this, so their silhouettes tint the pixels in front of them red.
float4 CullDebug(RasterVertex i) : SV_Target0
{
    if(!Accept(i.triangleIndex,i.bary,i.instance)) discard;
    return float4(1,0.12,0.12,1);
}
