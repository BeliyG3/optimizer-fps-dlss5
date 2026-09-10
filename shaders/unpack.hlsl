#include "fullscreen.hlsli"
#include "peripheral_warp_common.hlsli"

Texture2D<float4> PwPackedColor : register(t0);
Texture2D<float>  PwPackedDepth : register(t1);
Texture2D<float2> PwPackedMotion : register(t2);
Texture2D<float>  PwPackedConfidence : register(t3);
SamplerState PwLinearClamp : register(s0);
SamplerState PwPointClamp : register(s1);

PwFullscreenVertex VSMain(uint vertexId : SV_VertexID) { return PwFullscreenVS(vertexId); }

struct PwNativeOutput
{
    float4 color : SV_Target0;
    float depth : SV_Target1;
    float2 motion : SV_Target2;
    float confidence : SV_Target3;
};

PwNativeOutput PSMain(PwFullscreenVertex input)
{
    PwNativeOutput output;
    float2 nativePixel = input.position.xy;
    float2 workPixel = PwPackNativePixel(nativePixel);
    float2 workUv = workPixel / PwNativeWorkSize.zw;
    int2 guidePixel = clamp(int2(workPixel), int2(0, 0), int2(PwNativeWorkSize.zw) - 1);
    float2 guideCenter = float2(guidePixel) + 0.5;
    float2 packedMotion = PwPackedMotion.Load(int3(guidePixel, 0));

    output.color = PwPackedColor.SampleLevel(PwLinearClamp, workUv, 0.0);
    if (PwOptions.y == PW_FILTER_ADAPTIVE_FOUR_TAP)
    {
        // Soft cubic (B-spline) unpack where the packed texels are stretched: bilinear stretching
        // prints the packed texel grid as steps and diamonds; the B-spline (four bilinear fetches)
        // smooths them without ringing. Blends in above a footprint of 1 so the 1:1 zone stays exact.
        float2 footprint = PwSourceFootprint(workPixel);
        float soft = smoothstep(1.0, 1.3, max(footprint.x, footprint.y));
        if (soft > 0.0)
        {
            float2 texSize = PwNativeWorkSize.zw;
            float2 p = workPixel - 0.5;
            float2 i = floor(p), f = p - i;
            float2 f2 = f * f, f3 = f2 * f;
            float2 w0 = (1.0 - 3.0 * f + 3.0 * f2 - f3) / 6.0;
            float2 w1 = (3.0 * f3 - 6.0 * f2 + 4.0) / 6.0;
            float2 w2 = (-3.0 * f3 + 3.0 * f2 + 3.0 * f + 1.0) / 6.0;
            float2 w3 = f3 / 6.0;
            float2 s0 = w0 + w1, s1 = w2 + w3;
            float2 t0 = (i - 0.5 + w1 / s0) / texSize;
            float2 t1 = (i + 1.5 + w3 / s1) / texSize;
            float4 cubic = s0.x * s0.y * PwPackedColor.SampleLevel(PwLinearClamp, float2(t0.x, t0.y), 0.0) +
                           s1.x * s0.y * PwPackedColor.SampleLevel(PwLinearClamp, float2(t1.x, t0.y), 0.0) +
                           s0.x * s1.y * PwPackedColor.SampleLevel(PwLinearClamp, float2(t0.x, t1.y), 0.0) +
                           s1.x * s1.y * PwPackedColor.SampleLevel(PwLinearClamp, float2(t1.x, t1.y), 0.0);
            output.color = lerp(output.color, cubic, soft);
        }
    }
    // Output colour adjustment (host-set; 0 bits = not set): gain * pow(rgb, 1/gamma) in the
    // buffer's own linear space. Compensates the model's tone shift on a warped frame.
    if (PwDiagnosticOptions.y != 0u || PwDiagnosticOptions.z != 0u)
    {
        float gain = PwDiagnosticOptions.y != 0u ? asfloat(PwDiagnosticOptions.y) : 1.0;
        float invGamma = PwDiagnosticOptions.z != 0u ? asfloat(PwDiagnosticOptions.z) : 1.0;
        output.color.rgb = gain * pow(max(output.color.rgb, 0.0), invGamma);
    }
    output.depth = PwPackedDepth.Load(int3(guidePixel, 0));
    output.motion = PwUnpackMotionPixels(guideCenter, packedMotion);
    output.confidence = PwPackedConfidence.Load(int3(guidePixel, 0));
    float4 diagnosticColor;
    if (PwDiagnosticOutlineColor(nativePixel, diagnosticColor))
        output.color = diagnosticColor;
    return output;
}
