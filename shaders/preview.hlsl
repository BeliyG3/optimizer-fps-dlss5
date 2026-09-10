#include "fullscreen.hlsli"
#include "peripheral_warp_common.hlsli"

Texture2D<float4> PwPreviewColor : register(t0);
Texture2D<float2> PwPreviewMotion : register(t1);
SamplerState PwLinearClamp : register(s0);

cbuffer PwPreviewConstants : register(b1)
{
    uint PwPreviewMode; // 0 reconstructed, 1 density, 2 motion
    float PwMotionDisplayScale;
    float2 PwPreviewPadding;
};

PwFullscreenVertex VSMain(uint vertexId : SV_VertexID) { return PwFullscreenVS(vertexId); }

float3 PwMotionColor(float2 motion)
{
    float angle = atan2(motion.y, motion.x) * 0.159154943 + 0.5;
    float magnitude = saturate(length(motion) * PwMotionDisplayScale);
    float3 phase = frac(angle + float3(0.0, 0.6666667, 0.3333333));
    float3 hue = saturate(abs(phase * 6.0 - 3.0) - 1.0);
    return lerp(0.08.xxx, hue, magnitude);
}

float4 PSMain(PwFullscreenVertex input) : SV_Target0
{
    float2 nativePixel = input.position.xy;
    float2 workPixel = PwPackNativePixel(nativePixel);
    if (PwPreviewMode == 1)
    {
        float2 next = PwPackNativePixel(nativePixel + 1.0.xx);
        float density = sqrt(max(0.0, (next.x - workPixel.x) * (next.y - workPixel.y)));
        return float4(lerp(float3(1.0, 0.2, 0.0), float3(0.0, 0.8, 1.0), saturate(density)), 1.0);
    }
    float2 workUv = workPixel / PwNativeWorkSize.zw;
    if (PwPreviewMode == 2)
    {
        float2 motion = PwPreviewMotion.SampleLevel(PwLinearClamp, workUv, 0.0);
        return float4(PwMotionColor(PwUnpackMotionPixels(workPixel, motion)), 1.0);
    }
    return PwPreviewColor.SampleLevel(PwLinearClamp, workUv, 0.0);
}
