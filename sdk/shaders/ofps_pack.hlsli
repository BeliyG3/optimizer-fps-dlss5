#ifndef OFPS_PACK_HLSLI
#define OFPS_PACK_HLSLI

// The Pack stage as a reusable include: the four native inputs, the input-description constants and
// OfpsPackTexel, which produces one packed texel from a work-space pixel position. pack.hlsl wraps it
// in a full-screen pixel shader; an integrator that fuses Pack with its own encode pass includes this
// from a compute shader instead and gets byte-identical packed data.
//
// Requires ofps_common.hlsli. Register assignments can be overridden before inclusion:
//   OFPS_INPUT_CONSTANTS_REGISTER (default b1)

#include "ofps_common.hlsli"

#ifndef OFPS_INPUT_CONSTANTS_REGISTER
#define OFPS_INPUT_CONSTANTS_REGISTER b1
#endif

Texture2D<float4> OfpsSourceColor : register(t0);
Texture2D<float>  OfpsSourceDepth : register(t1);
Texture2D<float2> OfpsSourceMotion : register(t2);
Texture2D<float>  OfpsSourceConfidence : register(t3);
SamplerState OfpsLinearClamp : register(s0);
SamplerState OfpsPointClamp : register(s1);

cbuffer OfpsInputDescription : register(OFPS_INPUT_CONSTANTS_REGISTER)
{
    float4 OfpsColorRect;
    float4 OfpsDepthRect;
    float4 OfpsMotionRect;
    float4 OfpsConfidenceRect;
    float4 OfpsColorDepthSize;
    float4 OfpsMotionConfidenceSize;
    float2 OfpsInputMotionScale;
    float OfpsInputMotionDirectionSign;
    float OfpsInputFlags;
};

// Keeps a colour tap inside the colour region. The clamp sampler clamps only at the texture's edge, so
// when the region sits inside a larger texture (a render region in a full-size target) a tap past the
// region's edge would read the host's unused texels. For a region that fills its texture this is the
// clamp the sampler does anyway.
float2 OfpsClampColorUv(float2 uv)
{
    const float2 lo = (OfpsColorRect.xy + 0.5) / OfpsColorDepthSize.xy;
    const float2 hi = (OfpsColorRect.xy + OfpsColorRect.zw - 0.5) / OfpsColorDepthSize.xy;
    return clamp(uv, lo, hi);
}

float4 OfpsSamplePackedColor(float2 nativePixel, float2 workPixel)
{
    float2 nativeUv = nativePixel / OfpsNativeWorkSize.xy;
    float2 colorPixel = OfpsColorRect.xy + nativeUv * OfpsColorRect.zw;
    float2 uv = OfpsClampColorUv(colorPixel / OfpsColorDepthSize.xy);
    float4 color = 0.0.xxxx;
    if (OfpsOptions.y == OFPS_FILTER_ADAPTIVE_FOUR_TAP)
    {
        float2 footprint = OfpsSourceFootprint(workPixel);
        if (max(footprint.x, footprint.y) > 1.02)
        {
            // Wide pre-filter (tent-like, ~1.5 footprints): the centre tap plus four bilinear taps at
            // +-0.375 footprint. The former box of four taps at +-0.25 footprint covered exactly one
            // footprint, so a thin line crossing a texel boundary flickered as the taps jumped between
            // native texels. The spread fades in above a footprint of 1 so the 1:1 zone is untouched.
            float spread = 0.375 * saturate((max(footprint.x, footprint.y) - 1.0) / 0.3);
            float2 delta = spread * footprint * OfpsColorRect.zw /
                           (OfpsNativeWorkSize.xy * OfpsColorDepthSize.xy);
            color = 0.2 * (
                OfpsSourceColor.SampleLevel(OfpsLinearClamp, uv, 0.0) +
                OfpsSourceColor.SampleLevel(OfpsLinearClamp, OfpsClampColorUv(uv + float2(-delta.x, -delta.y)), 0.0) +
                OfpsSourceColor.SampleLevel(OfpsLinearClamp, OfpsClampColorUv(uv + float2( delta.x, -delta.y)), 0.0) +
                OfpsSourceColor.SampleLevel(OfpsLinearClamp, OfpsClampColorUv(uv + float2(-delta.x,  delta.y)), 0.0) +
                OfpsSourceColor.SampleLevel(OfpsLinearClamp, OfpsClampColorUv(uv + float2( delta.x,  delta.y)), 0.0));
        }
        else
            color = OfpsSourceColor.SampleLevel(OfpsLinearClamp, uv, 0.0);
    }
    else
        color = OfpsSourceColor.SampleLevel(OfpsLinearClamp, uv, 0.0);
    return color;
}

// One packed texel. workPixel is the texel centre in work space (integer + 0.5).
void OfpsPackTexel(float2 workPixel, out float4 color, out float depth, out float2 motion,
                 out float confidence)
{
    float2 nativePixel = OfpsUnpackWorkPixel(workPixel);
    float2 nativeUv = nativePixel / OfpsNativeWorkSize.xy;
    int2 depthPixel = int2(OfpsDepthRect.xy + nativeUv * OfpsDepthRect.zw);
    int2 motionPixel = int2(OfpsMotionRect.xy + nativeUv * OfpsMotionRect.zw);
    depthPixel = clamp(depthPixel, int2(OfpsDepthRect.xy),
                       int2(OfpsDepthRect.xy + OfpsDepthRect.zw) - 1);
    motionPixel = clamp(motionPixel, int2(OfpsMotionRect.xy),
                        int2(OfpsMotionRect.xy + OfpsMotionRect.zw) - 1);
    float2 nativeMotion = OfpsSourceMotion.Load(int3(motionPixel, 0)) *
                          OfpsInputMotionScale * OfpsInputMotionDirectionSign;

    color = OfpsSamplePackedColor(nativePixel, workPixel);
    depth = OfpsSourceDepth.Load(int3(depthPixel, 0));
    motion = OfpsPackMotionPixels(nativePixel, nativeMotion);
    confidence = 1.0;
    if ((uint(OfpsInputFlags) & 1u) != 0u)
    {
        float2 footprint = OfpsSourceFootprint(workPixel);
        int2 confidencePixel = int2(OfpsConfidenceRect.xy + nativeUv * OfpsConfidenceRect.zw);
        int2 delta = max(int2(1, 1), int2(ceil(
            0.25 * footprint * OfpsConfidenceRect.zw / OfpsNativeWorkSize.xy)));
        int2 lower = int2(OfpsConfidenceRect.xy);
        int2 upper = int2(OfpsConfidenceRect.xy + OfpsConfidenceRect.zw) - 1;
        float c0 = OfpsSourceConfidence.Load(int3(clamp(confidencePixel + int2(-delta.x, -delta.y), lower, upper), 0));
        float c1 = OfpsSourceConfidence.Load(int3(clamp(confidencePixel + int2( delta.x, -delta.y), lower, upper), 0));
        float c2 = OfpsSourceConfidence.Load(int3(clamp(confidencePixel + int2(-delta.x,  delta.y), lower, upper), 0));
        float c3 = OfpsSourceConfidence.Load(int3(clamp(confidencePixel + int2( delta.x,  delta.y), lower, upper), 0));
        confidence = min(min(c0, c1), min(c2, c3));
    }
}

#endif
