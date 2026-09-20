// Optimizer FPS: compute entry points for the SDK's temporal passes.
//
// The SDK's temporal machine (sdk/shaders/temporal.hlsl) is a set of full-screen pixel shaders. For
// the same reason as the warp (see warp_cs.hlsl) the fork runs them as compute: the pass records into
// the game's command list, and graphics draws left state behind that crashed the host. Both hosts use compute here.
//
// Nothing is reimplemented. Every entry point below builds the vertex a full-screen triangle would
// have produced for this texel and calls the SDK's pixel function; the pixel functions only read
// input.position.xy and use Load/SampleLevel, so they are valid in compute as they are.
// Build with tools\build-warp-shaders.cmd (one .dxbc per entry point).

#include "temporal_layout.h"

#define PW_T_EXPECT 1 // the depth a surface had in the residual's frame travels with the chain: t11 and PSExpect
#define PW_T_HISTORY 1 // rejected pixels look into the two full passes before the last: t12..t19
#define PW_T_CELLS 1 // rejected pixels are painted from cells of the accepted addition: PSCells
#define PW_T_RAMP 1 // a new pass is phased in over a few frames: t10 and PSResidualOld
#include "temporal.hlsl"

cbuffer PwTDispatch : register(b1)
{
    uint2 PwTTargetSize; // size of the texture this dispatch writes
    uint2 PwTFlowSize;   // size of the optical flow session's images (flow/OpticalFlow.h); 0 when unused
    uint PwTFlowGrid;    // session pixels per flow vector (1, 2 or 4)
    uint3 PwTDispatchPad;
};

// The optical flow engine's result: S10.5 fixed point, one vector per PwTFlowGrid block of the session's image,
// pointing from a pixel of the measured frame to where it is in the residual's frame.
Texture2D<int2> PwTFlow : register(t9);

RWTexture2D<float4> PwTOut0 : register(u0);
RWTexture2D<float4> PwTOut1 : register(u1); // the reprojection's addition + acceptance

PwFullscreenVertex PwTVertex(uint2 id)
{
    PwFullscreenVertex v;
    v.position = float4(float2(id) + 0.5, 0.0, 1.0);
    v.uv = (float2(id) + 0.5) / float2(PwTTargetSize);
    return v;
}

bool PwTOutside(uint2 id) { return id.x >= PwTTargetSize.x || id.y >= PwTTargetSize.y; }

// The frame is linear HDR here (the add-on worked on display-referred frames). Next to a lamp the
// model's contribution is tens of units per pixel, and carried one pixel off it lands on a surface
// worth 0.5: the sum goes negative and the lamp gets a black outline. A carried frame may therefore
// only move a pixel's LUMINANCE within a band around what the game rendered there: between a quarter
// of the pixel's own and 4 times the brightest of the pixel and its four neighbours two pixels away.
// Per channel the same band wrecked saturated colours (a green dress has next to no blue to scale).
// The full pass is shown through the same band (CSApply): a limit only the carried frames obeyed made
// every surface the model changes strongly click once per N frames.
float3 PwTLimitToFrame(float3 value, uint2 id)
{
    // The renodx bridge supplies display-referred frames; HDR limits only apply in ratio space.
    if (PwTSmooth.z <= 0.0) return value;
    const float3 kLuma = float3(0.2126, 0.7152, 0.0722);
    const int2 p = int2(PwTToRect(float2(id) + 0.5, PwTColorRect));
    const int2 last = int2(PwTColorRect.xy + PwTColorRect.zw) - 1;
    const float3 here = max(PwTColor.Load(int3(p, 0)).rgb, 0.0);
    float brightest = dot(here, kLuma);
    const int2 offsets[4] = { int2(2, 0), int2(-2, 0), int2(0, 2), int2(0, -2) };
    [unroll] for (int k = 0; k < 4; ++k)
        brightest = max(brightest, dot(max(PwTColor.Load(int3(clamp(p + offsets[k], int2(PwTColorRect.xy), last), 0)).rgb, 0.0), kLuma));

    value = max(value, 0.0);
    const float lumaHere = dot(here, kLuma);
    const float luma = dot(value, kLuma);
    const float low = 0.25 * lumaHere, high = 4.0 * brightest;
    if (luma > high && luma > 1e-6)
        value *= high / luma;
    else if (luma < low && lumaHere - luma > 1e-6)
        value = lerp(value, here, (low - luma) / (lumaHere - luma)); // back towards the rendered pixel
    return value;
}

[numthreads(PW_TEMPORAL_THREADS_X, PW_TEMPORAL_THREADS_Y, 1)]
void CSResidual(uint3 id : SV_DispatchThreadID)
{
    if (PwTOutside(id.xy)) return;
    PwTOut0[id.xy] = PSResidual(PwTVertex(id.xy));
}

[numthreads(PW_TEMPORAL_THREADS_X, PW_TEMPORAL_THREADS_Y, 1)]
void CSDownsample(uint3 id : SV_DispatchThreadID)
{
    if (PwTOutside(id.xy)) return;
    PwTOut0[id.xy] = PSDownsample(PwTVertex(id.xy));
}

[numthreads(PW_TEMPORAL_THREADS_X, PW_TEMPORAL_THREADS_Y, 1)]
void CSAccumulate(uint3 id : SV_DispatchThreadID)
{
    if (PwTOutside(id.xy)) return;
    PwTOut0[id.xy] = float4(PSAccumulate(PwTVertex(id.xy)), 0.0, 0.0);
}

// The engine's input: luminance of the frame bound at t0, 8 bit, at the session's size. The frames are
// linear HDR, so the luminance is taken in the ratio domain (a log) and spread over the 8 bits; the
// engine matches structure, and in linear light everything but the lamps would land in the first few
// codes.
[numthreads(PW_TEMPORAL_THREADS_X, PW_TEMPORAL_THREADS_Y, 1)]
void CSFlowLuma(uint3 id : SV_DispatchThreadID)
{
    if (PwTOutside(id.xy)) return;
    const float2 nativePixel = (float2(id.xy) + 0.5) / float2(PwTTargetSize) * PwTNative.xy;
    const float luma = PwTLumaNow(nativePixel); // bilinear at the centre of the footprint: a box over it
    PwTOut0[id.xy] = float4(saturate(luma / (PwTSmooth.z > 0.0 ? 9.0 : 1.0)), 0.0, 0.0, 0.0);
}

// The engine's field as a chain: for every motion texel, the displacement back to the residual's frame in
// the chain's own units (motion-texture pixels), bilinear between the four blocks around it.
[numthreads(PW_TEMPORAL_THREADS_X, PW_TEMPORAL_THREADS_Y, 1)]
void CSFlowChain(uint3 id : SV_DispatchThreadID)
{
    if (PwTOutside(id.xy)) return;
    const float2 toNative = PwTNative.xy / PwTMotionRect.zw;
    const float2 nativePixel = (float2(id.xy) + 0.5 - PwTMotionRect.xy) * toNative;
    const float2 fieldSize = float2(PwTFlowSize) / (float) max(PwTFlowGrid, 1u);
    const float2 g = nativePixel / PwTNative.xy * fieldSize - 0.5;
    const int2 base = int2(floor(g));
    const float2 t = g - float2(base);
    const int2 last = int2(fieldSize) - 1;
    float2 sum = 0.0;
    [unroll] for (int k = 0; k < 4; ++k)
    {
        const int2 o = int2(k & 1, k >> 1);
        const float w = (o.x == 0 ? 1.0 - t.x : t.x) * (o.y == 0 ? 1.0 - t.y : t.y);
        sum += float2(PwTFlow.Load(int3(clamp(base + o, int2(0, 0), last), 0))) * w;
    }
    // S10.5 -> session pixels -> native pixels -> motion-texture pixels.
    const float2 nativeShift = sum / 32.0 * (PwTNative.xy / float2(PwTFlowSize));
    PwTOut0[id.xy] = float4(nativeShift / toNative, 0.0, 0.0);
}

// A coarse sample of the chain for the CPU: how far, in native pixels, the picture has moved since the
// residual's frame, on a grid of PwTTargetSize points over the guides. Read back a few frames later to
// decide whether the model has to run before its turn (Temporal.cpp).
[numthreads(PW_TEMPORAL_THREADS_X, PW_TEMPORAL_THREADS_Y, 1)]
void CSStats(uint3 id : SV_DispatchThreadID)
{
    if (PwTOutside(id.xy)) return;
    const float2 uv = (float2(id.xy) + 0.5) / float2(PwTTargetSize);
    const float2 texel = PwTMotionRect.xy + uv * PwTMotionRect.zw;
    float2 acc = PwTAccPrev.Load(int3(int2(texel), 0));
    if (any(!isfinite(acc))) acc = float2(0.0, 0.0);
    PwTOut0[id.xy] = float4(length(acc * PwTNative.xy / PwTMotionRect.zw), 0.0, 0.0, 0.0);
}

[numthreads(PW_TEMPORAL_THREADS_X, PW_TEMPORAL_THREADS_Y, 1)]
void CSRefine(uint3 id : SV_DispatchThreadID)
{
    if (PwTOutside(id.xy)) return;
    PwTOut0[id.xy] = float4(PSRefine(PwTVertex(id.xy)), 0.0, 0.0);
}

[numthreads(PW_TEMPORAL_THREADS_X, PW_TEMPORAL_THREADS_Y, 1)]
void CSHistory(uint3 id : SV_DispatchThreadID)
{
    if (PwTOutside(id.xy)) return;
    const PwTHistoryOut o = PSHistory(PwTVertex(id.xy));
    PwTOut0[id.xy] = o.first;
    PwTOut1[id.xy] = o.second;
}

[numthreads(PW_TEMPORAL_THREADS_X, PW_TEMPORAL_THREADS_Y, 1)]
void CSCells(uint3 id : SV_DispatchThreadID)
{
    if (PwTOutside(id.xy)) return;
    const PwTCellsOut o = PSCells(PwTVertex(id.xy));
    PwTOut0[id.xy] = o.add;
    PwTOut1[id.xy] = o.look;
}

[numthreads(PW_TEMPORAL_THREADS_X, PW_TEMPORAL_THREADS_Y, 1)]
void CSExpect(uint3 id : SV_DispatchThreadID)
{
    if (PwTOutside(id.xy)) return;
    PwTOut0[id.xy] = PSExpect(PwTVertex(id.xy));
}

[numthreads(PW_TEMPORAL_THREADS_X, PW_TEMPORAL_THREADS_Y, 1)]
void CSResidualOld(uint3 id : SV_DispatchThreadID)
{
    if (PwTOutside(id.xy)) return;
    PwTOut0[id.xy] = PSResidualOld(PwTVertex(id.xy));
}

[numthreads(PW_TEMPORAL_THREADS_X, PW_TEMPORAL_THREADS_Y, 1)]
void CSModelMotion(uint3 id : SV_DispatchThreadID)
{
    if (PwTOutside(id.xy)) return;
    PwTOut0[id.xy] = float4(PSModelMotion(PwTVertex(id.xy)), 0.0, 0.0);
}

[numthreads(PW_TEMPORAL_THREADS_X, PW_TEMPORAL_THREADS_Y, 1)]
void CSReproject(uint3 id : SV_DispatchThreadID)
{
    if (PwTOutside(id.xy)) return;
    const PwTReprojectOut o = PSReproject(PwTVertex(id.xy));
    // Debug views are written as they are.
    PwTOut0[id.xy] = PwTParams.z == 0.0 ? float4(PwTLimitToFrame(PwTDecodeFrame(o.color.rgb), id.xy), o.color.a) : o.color;
    PwTOut1[id.xy] = o.addw;
}

// A full pass shown with the blended residual rather than the model's raw answer: the frame the model
// was given (t0) plus the residual this pass settled on (t2). Without it the blend only steadied the
// carried frames, and the full frame itself still snapped to the model's new version every N frames.
[numthreads(PW_TEMPORAL_THREADS_X, PW_TEMPORAL_THREADS_Y, 1)]
void CSApply(uint3 id : SV_DispatchThreadID)
{
    if (PwTOutside(id.xy)) return;
    const float4 frame = PwTLoadFrame(PwTColor, int3(int2(PwTToRect(float2(id.xy) + 0.5, PwTColorRect)), 0));
    float3 residual = PwTResidual.Load(int3(id.xy, 0)).rgb;
    if (PwTSmooth.w > 0.0 && PwTSmooth.w < 1.0)
    {
        const float3 older = PwTResidualOld.Load(int3(id.xy, 0)).rgb;
        if (all(isfinite(older))) residual = lerp(older, residual, PwTSmooth.w);
    }
    if (any(!isfinite(residual))) residual = 0.0;
    PwTOut0[id.xy] = float4(PwTLimitToFrame(PwTDecodeFrame(frame.rgb + residual), id.xy), frame.a);
}

[numthreads(PW_TEMPORAL_THREADS_X, PW_TEMPORAL_THREADS_Y, 1)]
void CSCompose(uint3 id : SV_DispatchThreadID)
{
    if (PwTOutside(id.xy)) return;
    const float4 composed = PSCompose(PwTVertex(id.xy));
    PwTOut0[id.xy] = float4(PwTLimitToFrame(PwTDecodeFrame(composed.rgb), id.xy), composed.a);
}
