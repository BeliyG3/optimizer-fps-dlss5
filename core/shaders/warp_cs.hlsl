#include "ofps_pack.hlsli"

RWTexture2D<float4> OfpsOutColor : register(u0);
RWTexture2D<float> OfpsOutDepth : register(u1);
RWTexture2D<float2> OfpsOutMotion : register(u2);

cbuffer OfpsDispatchRect : register(b3)
{
    uint4 OfpsOutputRect; // x/y origin in destination UAV, z/w native region extent
};

[numthreads(16, 8, 1)]
void CSPack(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= (uint)OfpsNativeWorkSize.z ||
        id.y >= (uint)OfpsNativeWorkSize.w)
        return;
    float4 color;
    float depth;
    float2 motion;
    float confidence;
    OfpsPackTexel(float2(id.xy) + 0.5, color, depth, motion, confidence);
    OfpsOutColor[id.xy] = color;
    OfpsOutDepth[id.xy] = depth;
    OfpsOutMotion[id.xy] = motion;
}

float4 OfpsSoftUnpack(float2 workPixel)
{
    float2 workUv = workPixel / OfpsNativeWorkSize.zw;
    float4 color = OfpsSourceColor.SampleLevel(OfpsLinearClamp, workUv, 0.0);
    if (OfpsOptions.y != OFPS_FILTER_ADAPTIVE_FOUR_TAP)
        return color;
    float2 footprint = OfpsSourceFootprint(workPixel);
    float soft = smoothstep(1.0, 1.3, max(footprint.x, footprint.y));
    if (soft <= 0.0)
        return color;
    float2 texSize = OfpsNativeWorkSize.zw;
    float2 p = workPixel - 0.5;
    float2 i = floor(p);
    float2 f = p - i;
    float2 f2 = f * f;
    float2 f3 = f2 * f;
    float2 w0 = (1.0 - 3.0 * f + 3.0 * f2 - f3) / 6.0;
    float2 w1 = (3.0 * f3 - 6.0 * f2 + 4.0) / 6.0;
    float2 w2 = (-3.0 * f3 + 3.0 * f2 + 3.0 * f + 1.0) / 6.0;
    float2 w3 = f3 / 6.0;
    float2 s0 = w0 + w1;
    float2 s1 = w2 + w3;
    float2 t0 = (i - 0.5 + w1 / s0) / texSize;
    float2 t1 = (i + 1.5 + w3 / s1) / texSize;
    float4 cubic =
        s0.x * s0.y * OfpsSourceColor.SampleLevel(OfpsLinearClamp, float2(t0.x, t0.y), 0.0) +
        s1.x * s0.y * OfpsSourceColor.SampleLevel(OfpsLinearClamp, float2(t1.x, t0.y), 0.0) +
        s0.x * s1.y * OfpsSourceColor.SampleLevel(OfpsLinearClamp, float2(t0.x, t1.y), 0.0) +
        s1.x * s1.y * OfpsSourceColor.SampleLevel(OfpsLinearClamp, float2(t1.x, t1.y), 0.0);
    return lerp(color, cubic, soft);
}

[numthreads(16, 8, 1)]
void CSUnpack(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= OfpsOutputRect.z || id.y >= OfpsOutputRect.w)
        return;
    float2 nativePixel = float2(id.xy) + 0.5;
    float2 workPixel = OfpsPackNativePixel(nativePixel);
    float4 color = OfpsSoftUnpack(workPixel);
    if (OfpsDiagnosticOptions.y != 0u || OfpsDiagnosticOptions.z != 0u)
    {
        float gain = OfpsDiagnosticOptions.y != 0u
            ? asfloat(OfpsDiagnosticOptions.y) : 1.0;
        float invGamma = OfpsDiagnosticOptions.z != 0u
            ? asfloat(OfpsDiagnosticOptions.z) : 1.0;
        color.rgb = gain * pow(max(color.rgb, 0.0), invGamma);
    }
    float4 outline;
    if (OfpsDiagnosticOutlineColor(nativePixel, outline))
        color = outline;
    OfpsOutColor[OfpsOutputRect.xy + id.xy] = color;
}
