// Optimizer FPS: Pack as a compute shader.
//
// For hosts that evaluate the model on a compute command list (DLSS5-Reshade-AIO with asynchronous
// NGX compute), where the full-screen draws of pack.hlsl cannot be recorded. Nothing is
// reimplemented: every thread calls the SDK's PwPackTexel with the pixel centre a full-screen
// triangle would have produced, so the packed data is the same as pack.hlsl's.

#include "peripheral_warp_common.hlsli"
#include "peripheral_warp_pack.hlsli"

#define PW_WARP_THREADS_X 16
#define PW_WARP_THREADS_Y 8

cbuffer PwWarpDispatch : register(b3)
{
    uint2 PwWTargetSize; // size of the textures this dispatch writes
    uint2 PwWDispatchPad;
};

RWTexture2D<float4> PwWColor : register(u0);
RWTexture2D<float>  PwWDepth : register(u1);
RWTexture2D<float2> PwWMotion : register(u2);
RWTexture2D<float>  PwWConfidence : register(u3);

[numthreads(PW_WARP_THREADS_X, PW_WARP_THREADS_Y, 1)]
void CSPack(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= PwWTargetSize.x || id.y >= PwWTargetSize.y) return;
    float4 color;
    float depth, confidence;
    float2 motion;
    PwPackTexel(float2(id.xy) + 0.5, color, depth, motion, confidence);
    PwWColor[id.xy] = color;
    PwWDepth[id.xy] = depth;
    PwWMotion[id.xy] = motion;
    PwWConfidence[id.xy] = confidence;
}
