#ifndef PERIPHERAL_WARP_PACK_HLSLI
#define PERIPHERAL_WARP_PACK_HLSLI

// The Pack stage as a reusable include: the four native inputs, the input-description constants and
// PwPackTexel, which produces one packed texel from a work-space pixel position. pack.hlsl wraps it
// in a full-screen pixel shader; an integrator that fuses Pack with its own encode pass includes this
// from a compute shader instead and gets byte-identical packed data.
//
// Requires peripheral_warp_common.hlsli. Register assignments can be overridden before inclusion:
//   PW_INPUT_CONSTANTS_REGISTER (default b1)

#include "peripheral_warp_common.hlsli"

#ifndef PW_INPUT_CONSTANTS_REGISTER
#define PW_INPUT_CONSTANTS_REGISTER b1
#endif

Texture2D<float4> PwSourceColor : register(t0);
Texture2D<float>  PwSourceDepth : register(t1);
Texture2D<float2> PwSourceMotion : register(t2);
Texture2D<float>  PwSourceConfidence : register(t3);
SamplerState PwLinearClamp : register(s0);
SamplerState PwPointClamp : register(s1);

cbuffer PwInputDescription : register(PW_INPUT_CONSTANTS_REGISTER)
{
    float4 PwColorRect;
    float4 PwDepthRect;
    float4 PwMotionRect;
    float4 PwConfidenceRect;
    float4 PwColorDepthSize;
    float4 PwMotionConfidenceSize;
    float2 PwInputMotionScale;
    float PwInputMotionDirectionSign;
    float PwInputFlags;
};

float4 PwSamplePackedColor(float2 nativePixel, float2 workPixel)
{
    float2 nativeUv = nativePixel / PwNativeWorkSize.xy;
    float2 colorPixel = PwColorRect.xy + nativeUv * PwColorRect.zw;
    float2 uv = colorPixel / PwColorDepthSize.xy;
    float4 color = 0.0.xxxx;
    if (PwOptions.y == PW_FILTER_ADAPTIVE_FOUR_TAP)
    {
        float2 footprint = PwSourceFootprint(workPixel);
        if (max(footprint.x, footprint.y) > 1.02)
        {
            // Wide pre-filter (tent-like, ~1.5 footprints): the centre tap plus four bilinear taps at
            // +-0.375 footprint. The former box of four taps at +-0.25 footprint covered exactly one
            // footprint, so a thin line crossing a texel boundary flickered as the taps jumped between
            // native texels. The spread fades in above a footprint of 1 so the 1:1 zone is untouched.
            float spread = 0.375 * saturate((max(footprint.x, footprint.y) - 1.0) / 0.3);
            float2 delta = spread * footprint * PwColorRect.zw /
                           (PwNativeWorkSize.xy * PwColorDepthSize.xy);
            color = 0.2 * (
                PwSourceColor.SampleLevel(PwLinearClamp, uv, 0.0) +
                PwSourceColor.SampleLevel(PwLinearClamp, uv + float2(-delta.x, -delta.y), 0.0) +
                PwSourceColor.SampleLevel(PwLinearClamp, uv + float2( delta.x, -delta.y), 0.0) +
                PwSourceColor.SampleLevel(PwLinearClamp, uv + float2(-delta.x,  delta.y), 0.0) +
                PwSourceColor.SampleLevel(PwLinearClamp, uv + float2( delta.x,  delta.y), 0.0));
        }
        else
            color = PwSourceColor.SampleLevel(PwLinearClamp, uv, 0.0);
    }
    else
        color = PwSourceColor.SampleLevel(PwLinearClamp, uv, 0.0);
    return color;
}

// One packed texel. workPixel is the texel centre in work space (integer + 0.5).
void PwPackTexel(float2 workPixel, out float4 color, out float depth, out float2 motion,
                 out float confidence)
{
    float2 nativePixel = PwUnpackWorkPixel(workPixel);
    float2 nativeUv = nativePixel / PwNativeWorkSize.xy;
    int2 depthPixel = int2(PwDepthRect.xy + nativeUv * PwDepthRect.zw);
    int2 motionPixel = int2(PwMotionRect.xy + nativeUv * PwMotionRect.zw);
    depthPixel = clamp(depthPixel, int2(PwDepthRect.xy),
                       int2(PwDepthRect.xy + PwDepthRect.zw) - 1);
    motionPixel = clamp(motionPixel, int2(PwMotionRect.xy),
                        int2(PwMotionRect.xy + PwMotionRect.zw) - 1);
    float2 nativeMotion = PwSourceMotion.Load(int3(motionPixel, 0)) *
                          PwInputMotionScale * PwInputMotionDirectionSign;

    color = PwSamplePackedColor(nativePixel, workPixel);
    depth = PwSourceDepth.Load(int3(depthPixel, 0));
    motion = PwPackMotionPixels(nativePixel, nativeMotion);
    confidence = 1.0;
    if ((uint(PwInputFlags) & 1u) != 0u)
    {
        float2 footprint = PwSourceFootprint(workPixel);
        int2 confidencePixel = int2(PwConfidenceRect.xy + nativeUv * PwConfidenceRect.zw);
        int2 delta = max(int2(1, 1), int2(ceil(
            0.25 * footprint * PwConfidenceRect.zw / PwNativeWorkSize.xy)));
        int2 lower = int2(PwConfidenceRect.xy);
        int2 upper = int2(PwConfidenceRect.xy + PwConfidenceRect.zw) - 1;
        float c0 = PwSourceConfidence.Load(int3(clamp(confidencePixel + int2(-delta.x, -delta.y), lower, upper), 0));
        float c1 = PwSourceConfidence.Load(int3(clamp(confidencePixel + int2( delta.x, -delta.y), lower, upper), 0));
        float c2 = PwSourceConfidence.Load(int3(clamp(confidencePixel + int2(-delta.x,  delta.y), lower, upper), 0));
        float c3 = PwSourceConfidence.Load(int3(clamp(confidencePixel + int2( delta.x,  delta.y), lower, upper), 0));
        confidence = min(min(c0, c1), min(c2, c3));
    }
}

#endif
