#pragma once
#include "math.h"
#include "shaders/raster_depth.hlsli"

// --ao's helpers on the CPU, mirrored in shaders/raster_ao_math.hlsli (the CPU checks in tests_raster.cpp hold these).

// View depth (clip w) of a depth-buffer value written with math.h's Perspective (near n = 0.1, far f = 300).
// Standard: n*f/(f - d*(f-n)); reverse (the reversed projection): n*f/(n + d*(f-n)).
inline float LinearDepth(float d, bool reverse) { return ViewDepthFromHardware(d,reverse); }

// A unit normal on the octahedron |x|+|y|+|z| = 1, its lower half folded out: two numbers in [-1,1] (the prepass
// stores them as RG16_SNORM).
struct OctNormal { float x=0, y=0; };
inline OctNormal OctEncode(Vec3 n)
{
    const float sum=std::abs(n.x)+std::abs(n.y)+std::abs(n.z), x=n.x/sum, y=n.y/sum;
    if(n.z>=0) return {x,y};
    return {(1-std::abs(y))*(x>=0 ? 1.0f : -1.0f),(1-std::abs(x))*(y>=0 ? 1.0f : -1.0f)};
}
inline Vec3 OctDecode(OctNormal e)
{
    Vec3 n{e.x,e.y,1-std::abs(e.x)-std::abs(e.y)};
    const float fold=std::max(-n.z,0.0f);
    n.x+=n.x>=0 ? -fold : fold; n.y+=n.y>=0 ? -fold : fold;
    return Normalize(n);
}

// The AO sample pattern: a 4x4 interleaved (Bayer) index per pixel, so every 4x4 window - the blur's - holds each of
// the 16 values once. `offset` shifts it (0 for --ao clean; game mode uses AoGameNoise instead, so the offset is only exercised by the tests). rotation turns
// the slice directions by that fraction of their spacing; step shifts the steps along each slice, from the index with
// its two base-4 digits swapped, so the two do not move together.
struct AoInterleave { float rotation=0, step=0; };
inline unsigned AoPatternIndex(unsigned x, unsigned y, unsigned offset)
{
    static constexpr unsigned bayer[4][4]={{0,8,2,10},{12,4,14,6},{3,11,1,9},{15,7,13,5}};
    return (bayer[y%4][x%4]+offset)%16;
}
inline AoInterleave AoPattern(unsigned x, unsigned y, unsigned offset)
{
    const unsigned i=AoPatternIndex(x,y,offset);
    return {(float(i)+0.5f)/16,(float((i%4)*4+i/4)+0.5f)/16};
}

// --ao game's per-pixel noise: interleaved gradient noise (Jimenez 2014), IGN(x,y) = frac(52.9829189*frac(0.06711056*x
// + 0.00583715*y)), its position moved by 5.588238 pixels per frame (64 frames, then it repeats). Unlike AoPattern's
// shifted index, a 4x4 window does not hold the same values every frame, so the 2x2 blur leaves per-frame noise
// that the temporal filter averages. rotation: the slice rotation; step: the step offset, from IGN of the transposed
// pixel (a shift of the same IGN would only add a constant to rotation).
inline float AoIgn(float x, float y)
{
    const float inner=0.06711056f*x+0.00583715f*y;
    const float outer=52.9829189f*(inner-std::floor(inner));
    return outer-std::floor(outer);
}
inline AoInterleave AoGameNoise(unsigned x, unsigned y, unsigned frame)
{
    const float t=5.588238f*float(frame%64);
    return {AoIgn(float(x)+t,float(y)+t),AoIgn(float(y)+t,float(x)+t)};
}

// --ao game's temporal filter (shaders/raster_ao_temporal.hlsl): out = lerp(history, current, weight). The weight is 1
// (the current AO alone) when the history is not valid or the reprojected sample is rejected, else alpha (--ao-alpha,
// 0.1 by default; 1 keeps no history).
inline float AoHistoryWeight(bool valid, bool rejected, float alpha=0.1f) { return valid && !rejected ? alpha : 1.0f; }
// The history holds the previous frame's AO only when the targets were not (re)created since (create, resize,
// --render-switch-at into raster) and the last AO frame was the one right before this one (a path-traced frame between
// them leaves it stale). previousFrame: -1 before the first AO frame.
inline bool AoHistoryUsable(bool targetsRecreated, int previousFrame, int frame)
{
    return !targetsRecreated && previousFrame>=0 && frame==previousFrame+1;
}
