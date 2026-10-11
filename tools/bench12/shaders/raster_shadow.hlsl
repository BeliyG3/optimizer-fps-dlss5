// --render raster, sun shadow pass: depth only, orthographic from the sun. Alpha-cutoff texels are clipped as in
// the scene pass, so foliage casts its cut-out shadow. Compiled with PW_LIGHTMAP=1 (raster_shadow_dynamic_vs.cso) for
// --lightmap's second map: the draw covers the dynamic triangles alone, which follow the static ones in `vertices`
// (PW_SHADOW_LIST below when static triangles have no lightmap UVs). PW_LAMPSHADOW: the lamp groups' spot maps.
#include "raster_common.hlsli"
#if PW_CSM
// --csm (raster_csm_shadow_vs.cso, raster_csm_shadow_dynamic_vs.cso): one draw per cascade into its array slice.
#include "raster_csm.hlsli"
cbuffer RasterCascadePass : register(b3,space1) { uint cascadeIndex; };
#endif
#if PW_SHADOW_LIST
// The baked mode's dynamic-only maps when static triangles lack lightmap UVs (veg_ foliage without --wind), and every
// --lamp-shadows dynamic-only map: the draw walks Scene::unbakedTriangles, the triangles without baked light
// (raster_shadow_list_vs.cso, raster_csm_shadow_list_vs.cso, raster_lampshadow_list_vs.cso).
StructuredBuffer<uint> shadowTriangles : register(t2,space3);
#endif
#if PW_LAMPSHADOW
// --lamp-shadows (raster_lampshadow[_list]_vs.cso): one draw per lamp group into its slice (raster_lamp_shadow.hlsli,
// included by raster_common.hlsli).
cbuffer RasterLampShadowPass : register(b1,space7) { uint lampShadowPassSlice; };
#endif
struct ShadowVertex {
    float4 position : SV_Position;
    float2 bary : BARYCENTRIC;
    nointerpolation uint triangleIndex : TRIANGLE;
    nointerpolation uint instance : INSTANCEINDEX;
};
// `instance`: as raster.hlsl's VS (raster_instances.h); the list and dynamic variants draw flattened triangles alone.
ShadowVertex VS(uint id : SV_VertexID, uint instance : INSTANCE)
{
#if PW_SHADOW_LIST
    id=shadowTriangles[id/3]*3+id%3;
#elif PW_LIGHTMAP
    id+=dynamicBase*3;
#endif
    id=StoredVertex(id,instance);
    ShadowVertex o; uint corner=id%3; float3 position=WorldPosition(id,instance);
#if PW_LAMPSHADOW
    o.position=mul(lampShadowVP[lampShadowPassSlice],float4(position,1));
#elif PW_CSM
    o.position=mul(cascadeVP[cascadeIndex],float4(position,1));
#else
    o.position=mul(sunVP,float4(position,1));
#endif
    o.bary=float2(corner==1 ? 1 : 0,corner==2 ? 1 : 0); o.triangleIndex=id/3; o.instance=instance;
    return o;
}
void PS(ShadowVertex i)
{
    if(!Accept(i.triangleIndex,i.bary,i.instance)) discard;
}
