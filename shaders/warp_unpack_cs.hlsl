// Optimizer FPS: the colour-only Unpack as a compute shader (see warp_pack_cs.hlsl).

#include "peripheral_warp_common.hlsli"
#include "peripheral_warp_unpack.hlsli"

#define PW_WARP_THREADS_X 16
#define PW_WARP_THREADS_Y 8

cbuffer PwWarpDispatch : register(b3)
{
    uint2 PwWTargetSize; // size of the texture this dispatch writes
    uint2 PwWDispatchPad;
};

RWTexture2D<float4> PwWColor : register(u0);

[numthreads(PW_WARP_THREADS_X, PW_WARP_THREADS_Y, 1)]
void CSUnpackColor(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= PwWTargetSize.x || id.y >= PwWTargetSize.y) return;
    float4 color;
    float depth, confidence;
    float2 motion;
    PwUnpackTexel(float2(id.xy) + 0.5, color, depth, motion, confidence);
    PwWColor[id.xy] = color;
}
