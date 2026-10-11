#pragma once
#include "math.h"
#include <climits>

// The per-frame constants both renderers read (frame.hlsli, register b0).
// Each row after the matrices is one HLSL constant-register (16 bytes).
struct FrameConstants {
    Mat4 currentVP, previousVP;
    Vec3 eye; float tanHalf;
    Vec3 right; float aspect;
    Vec3 up; float exposure;
    Vec3 forward; float albedo;
    Vec3 sunTravel; float sunStrength;
    unsigned width, height, frame, spp;
    unsigned bounces, opaqueTriangles, lightCount, accumulation;
    float jitterX, jitterY, firefly, sunCos;
    float lightPower; unsigned reverse, view, lightCandidates;
    float hazeDensity, hazeG, bloom; unsigned neutralTonemap; // 0 ACES filmic, 1 neutral, 2 none, 3 UE filmic
    int pickX=-1, pickY=-1; unsigned accumulationLimit=UINT_MAX, padding=0;
    unsigned dynamicBase=0, dynamicOpaque=0, motionNdc=0, animationPadding=0;
    float filmSlope=0.88f, filmToe=0.55f, filmShoulder=0.26f, filmBlackClip=0; // --tonemap ue-filmic (tonemap_ue.hlsli)
    float filmWhiteClip=0.04f, localHighlights=1, localShadows=1, localDetail=1; // --local-exposure (local_exposure.hlsli)
    float localBlend=0.6f; unsigned localExposure=0, displayPadding0=0, displayPadding1=0;
};
static_assert(sizeof(FrameConstants)==368);
