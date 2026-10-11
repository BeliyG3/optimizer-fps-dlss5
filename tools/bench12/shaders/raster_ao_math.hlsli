#ifndef PW_RASTER_AO_MATH_HLSLI
#define PW_RASTER_AO_MATH_HLSLI
// --ao's helpers, the HLSL mirror of ao_math.h (whose CPU checks cover them): view depth from the depth buffer in
// either convention, and the octahedral normal encoding of the prepass (raster_prepass.hlsl, raster_ao.hlsl).
#include "raster_depth.hlsli"
float LinearDepth(float d, bool reverse) { return ViewDepthFromHardware(d,reverse); }
float2 OctEncode(float3 n)
{
    n/=abs(n.x)+abs(n.y)+abs(n.z);
    if(n.z>=0) return n.xy;
    return (1-abs(n.yx))*float2(n.x>=0 ? 1.0 : -1.0,n.y>=0 ? 1.0 : -1.0);
}
float3 OctDecode(float2 e)
{
    float3 n=float3(e,1-abs(e.x)-abs(e.y));
    float fold=max(-n.z,0);
    n.x+=n.x>=0 ? -fold : fold; n.y+=n.y>=0 ? -fold : fold;
    return normalize(n);
}
// The AO sample pattern (ao_math.h's AoPattern): x the slice rotation, y the step offset, both in (0,1), from a 4x4
// interleaved index that the 4x4 blur sees each value of once. offset: 0 for --ao clean.
float2 AoPattern(uint2 pixel, uint offset)
{
    static const uint bayer[16]={0,8,2,10,12,4,14,6,3,11,1,9,15,7,13,5};
    uint i=(bayer[(pixel.y%4)*4+pixel.x%4]+offset)%16;
    return float2((i+0.5)/16,((i%4)*4+i/4+0.5)/16);
}
// --ao game's per-pixel noise (ao_math.h's AoGameNoise): interleaved gradient noise moved 5.588238 pixels per frame;
// x the slice rotation, y the step offset (IGN of the transposed pixel).
float AoIgn(float2 p) { return frac(52.9829189*frac(dot(p,float2(0.06711056,0.00583715)))); }
float2 AoGameNoise(uint2 pixel, uint frame)
{
    float t=5.588238*(frame%64);
    return float2(AoIgn(float2(pixel)+t),AoIgn(float2(pixel.yx)+t));
}
// --ao game's temporal weight (ao_math.h's AoHistoryWeight): the current AO alone without a usable history sample.
float AoHistoryWeight(bool valid, bool rejected, float alpha) { return valid && !rejected ? alpha : 1.0; }
#endif
