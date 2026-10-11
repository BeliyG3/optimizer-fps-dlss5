#pragma once
// The per-vertex math of shaders/skin.hlsl in C++, line for line, for the unit test against the CPU path (tests_skinning.cpp).
#include "skin_layout.h"
#include "wind.h"
#include <cmath>

// The wind's two sines at a bench frame (WindOffset's phases), in double as WindOffset evaluates them: the shader adds the
// vertex's own phase k * p by the angle-sum identity.
struct SkinWindPhase { float sinA=0, cosA=1, sinB=0, cosB=1; };
inline SkinWindPhase WindPhase(unsigned frame)
{
    const double t=double(frame)/60.0;
    return {float(std::sin(kWindOmega*t)),float(std::cos(kWindOmega*t)),float(std::sin(2.7*kWindOmega*t)),float(std::cos(2.7*kWindOmega*t))};
}
inline Vec3 SkinWindOffset(const SkinWindPhase &phase, Vec3 position, float weight, float strength)
{
    if(weight==0 || strength==0) return {};
    const float p=position.x*kWindDirX+position.z*kWindDirZ, a=kWindWave*p, b=1.3f*kWindWave*p;
    const float sway=kWindAmplitude*(phase.sinA*std::cos(a)+phase.cosA*std::sin(a))+kWindFlutter*(phase.sinB*std::cos(b)+phase.cosB*std::sin(b));
    const float s=strength*weight*sway;
    return {kWindDirX*s,0,kWindDirZ*s};
}
struct SkinFrame { const Affine3x4 *palette; const float *weights; SkinWindPhase wind; };
// Item vertex `g` at one frame: its bench-space position (wind added) and, when `normal` is not null, its normal.
inline Vec3 SkinVertex(const SkinLayout &layout, unsigned g, const SkinFrame &frame, float windStrength, Vec3 *normal)
{
    const SkinItem &item=layout.items[layout.vertexItem[g]];
    const unsigned v=g-item.firstVertex;
    const SkinSourceVertex &s=layout.sources[item.source+v];
    Vec3 p=s.position;
    for(unsigned k=0;k<item.targetCount;++k) {
        const float w=frame.weights[item.weights+k];
        if(w!=0) p=p+layout.targets[item.targets+k*item.vertexCount+v]*w;
    }
    Affine3x4 blend; float total=0;
    const unsigned entries[4]={s.joints[0]&0xffffu,s.joints[0]>>16,s.joints[1]&0xffffu,s.joints[1]>>16};
    for(unsigned k=0;k<4;++k) if(s.weights[k]!=0) { blend.Add(frame.palette[item.palette+entries[k]],s.weights[k]); total+=s.weights[k]; }
    Vec3 position=blend.Point(p)*(1/total);
    if(normal) *normal=Normalize(blend.Direction(s.normal)); // zero stays zero, as the shader's guard
    if(item.wind) position=position+SkinWindOffset(frame.wind,position,layout.wind[g],windStrength);
    return position;
}
