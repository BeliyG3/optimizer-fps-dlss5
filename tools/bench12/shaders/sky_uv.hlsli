#ifndef PW_SKY_UV_HLSLI
#define PW_SKY_UV_HLSLI
// The sky panorama's (environment, t3) texture coordinate of a direction: longitude atan2(z, x) across, latitude acos(y)
// down, the latitude clamped to the texel centres of `rows` (longitude alone is periodic). Sky() in scene.hlsli (the path
// tracer's miss and the raster sky) and the sky reflections' cube (sky_prefilter.hlsl) read the panorama through it.
float2 SkyUv(float3 direction, uint rows)
{
    float2 uv=float2(atan2(direction.z,direction.x)/(2*3.14159265359)+0.5,acos(clamp(direction.y,-1,1))/3.14159265359);
    uv.y=clamp(uv.y,0.5/rows,1-0.5/rows);
    return uv;
}
#endif
