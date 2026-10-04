#include "ofps_pack.hlsli"

RWTexture2D<float4> OfpsOutColor : register(u0);
RWTexture2D<float> OfpsOutDepth : register(u1);
RWTexture2D<float2> OfpsOutMotion : register(u2);

cbuffer OfpsDispatchRect : register(b3)
{
    uint4 OfpsOutputRect; // x/y origin in destination UAV, z/w native region extent
};

// Detail transfer (colour filters 2/3). t4: the packed colour the model received (work size);
// t5/t6: the colour and depth the Pack read (inside OfpsColorRect / OfpsDepthRect of their textures).
Texture2D<float4> OfpsModelInput : register(t4);
Texture2D<float4> OfpsFullColor : register(t5);
Texture2D<float>  OfpsFullDepth : register(t6);

// OfpsDiagnosticOptions.w: bit 0 depth is reversed (near = 1), bit 1 transfer inputs are bound,
// bit 2 the Peripheral band is exact 1:1 (work extent equals the raw work extent, GlobalScale 100).
static const uint OFPS_TRANSFER_DEPTH_REVERSED = 1u;
static const uint OFPS_TRANSFER_BOUND = 2u;
static const uint OFPS_TRANSFER_BAND_EXACT = 4u;
// Relative depth difference at which a work texel's edit counts half. Depth in the near = 1 form tracks
// 1/z for both conventions, so the same tolerance means the same near and far. Tuned on bench12.
static const float OFPS_TRANSFER_DEPTH_TOLERANCE = 0.05;

float OfpsNearIsOne(float d)
{
    return (OfpsDiagnosticOptions.w & OFPS_TRANSFER_DEPTH_REVERSED) != 0u ? d : 1.0 - d;
}

int2 OfpsFullTexel(float2 nativePixel, float4 rect)
{
    const float2 uv = nativePixel / OfpsNativeWorkSize.xy;
    const int2 texel = int2(rect.xy + uv * rect.zw);
    return clamp(texel, int2(rect.xy), int2(rect.xy + rect.zw) - 1);
}

// The model's edit (all four channels, alpha included) at a full-size pixel: its answer minus what it
// was given. Plain: bilinear over the four work texels around the sample point. Depth-guided: a tent of
// radius 2 texels over the 4x4 texels around it, each weight scaled by how close the texel's depth is to
// this pixel's. When no texel lies on this pixel's surface (a thin object the reduced grid missed), the
// edit is zero: the pixel keeps the frame's own value instead of borrowing another surface's edit.
float4 OfpsTransferEdit(float2 workPixel, float pixelDepth, bool depthGuided)
{
    const float2 p = workPixel - 0.5;
    const float2 base = floor(p);
    const float2 f = p - base;
    const int2 maxTexel = int2(OfpsNativeWorkSize.zw) - 1;
    float4 sum = 0.0;
    float weightSum = 0.0;
    float4 edit = 0.0;
    // One return at the end: fxc reports an early return from this branch as a use of an
    // uninitialised variable (X4000).
    if (!depthGuided) {
        [unroll] for (int k = 0; k < 4; ++k) {
            const int2 o = int2(k & 1, k >> 1);
            const int2 t = clamp(int2(base) + o, int2(0, 0), maxTexel);
            const float w = (o.x != 0 ? f.x : 1.0 - f.x) * (o.y != 0 ? f.y : 1.0 - f.y);
            sum += (OfpsSourceColor.Load(int3(t, 0)) - OfpsModelInput.Load(int3(t, 0))) * w;
            weightSum += w;
        }
        edit = sum / max(weightSum, 1.0e-6);
    } else {
        float spatialWeightSum = 0.0;
        [unroll] for (int j = 0; j < 16; ++j) {
            const int2 o = int2(j & 3, j >> 2) - 1;                   // -1..2 around the bilinear pair
            const int2 t = clamp(int2(base) + o, int2(0, 0), maxTexel);
            const float2 dist = abs(float2(o) - f);                  // texel centre to sample point, in texels
            const float tent = saturate(1.0 - dist.x * 0.5) * saturate(1.0 - dist.y * 0.5);
            const float d = OfpsNearIsOne(OfpsSourceDepth.Load(int3(t, 0)));
            const float mismatch = abs(d - pixelDepth) / max(max(d, pixelDepth), 1.0e-4);
            const float r = mismatch / OFPS_TRANSFER_DEPTH_TOLERANCE;
            const float w = tent * exp2(-r * r);
            sum += (OfpsSourceColor.Load(int3(t, 0)) - OfpsModelInput.Load(int3(t, 0))) * w;
            weightSum += w;
            spatialWeightSum += tent;
        }
        // Full strength while at least a quarter of the tent's weight lies on this pixel's surface; below
        // that the edit fades towards zero instead of normalising a poorly matched neighbour back up.
        // spatialWeightSum is always 4 (the separable tent sums to 2 per axis regardless of the
        // subpixel offset f, 2 x 2 = 4 overall), so the floor 0.25 * spatialWeightSum is the
        // constant 1.0: tuning 0.25 moves the fade threshold itself, not a coverage ratio.
        if (weightSum > 1.0e-3)
            edit = sum / max(weightSum, 0.25 * spatialWeightSum);
    }
    return edit;
}

// Inside the Peripheral band the plain unpack is exact while the band is 1:1 (OFPS_TRANSFER_BAND_EXACT);
// the transfer then never runs there, also for the pixels next to a fractional band edge whose footprint
// already reaches the compressed side. With GlobalScale < 100 the band is shrunk like the rest of the
// frame, so it is not excluded and transfers wherever its footprint exceeds 1.
bool OfpsInCenterBand(float2 nativePixel)
{
    if (OfpsOptions.x != OFPS_MODE_PERIPHERAL) return false; // Uniform has no band
    if ((OfpsDiagnosticOptions.w & OFPS_TRANSFER_BAND_EXACT) == 0u) return false;
    float2 lo, hi;
    OfpsCenterRectangle(lo, hi);
    return OfpsRectangleSignedDistance(nativePixel, lo, hi) <= 0.0;
}

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
    if (OfpsOptions.y < OFPS_FILTER_ADAPTIVE_FOUR_TAP)
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
    const float2 footprint = OfpsSourceFootprint(workPixel);
    const bool useTransfer = OfpsOptions.y >= OFPS_FILTER_DETAIL_TRANSFER &&
                             (OfpsDiagnosticOptions.w & OFPS_TRANSFER_BOUND) != 0u &&
                             !OfpsInCenterBand(nativePixel) && max(footprint.x, footprint.y) > 1.001;
    if (useTransfer) {
        const float4 full = OfpsFullColor.Load(int3(OfpsFullTexel(nativePixel, OfpsColorRect), 0));
        const float pixelDepth = OfpsNearIsOne(OfpsFullDepth.Load(int3(OfpsFullTexel(nativePixel, OfpsDepthRect), 0)));
        // No clamp: a model that changes nothing must give back the frame exactly (HDR negatives
        // included); the gain/gamma adjustment below clamps only when it changes the colour.
        color = full + OfpsTransferEdit(workPixel, pixelDepth,
                                         OfpsOptions.y == OFPS_FILTER_DETAIL_TRANSFER_DEPTH);
    }
    if (OfpsDiagnosticOptions.y != 0u || OfpsDiagnosticOptions.z != 0u)
    {
        float gain = OfpsDiagnosticOptions.y != 0u
            ? asfloat(OfpsDiagnosticOptions.y) : 1.0;
        float invGamma = OfpsDiagnosticOptions.z != 0u
            ? asfloat(OfpsDiagnosticOptions.z) : 1.0;
        // The host always sets the adjustment (gain 1, gamma 1 at default brightness). While a transfer is
        // bound for this dispatch that identity is skipped on every pixel, transferred or not (the 1:1 band
        // too), so HDR negatives survive without a seam at the band edge. Without a bound transfer every
        // filter clamps as before.
        if ((OfpsDiagnosticOptions.w & OFPS_TRANSFER_BOUND) == 0u || gain != 1.0 || invGamma != 1.0)
            color.rgb = gain * pow(max(color.rgb, 0.0), invGamma);
    }
    float4 outline;
    if (OfpsDiagnosticOutlineColor(nativePixel, outline))
        color = outline;
    OfpsOutColor[OfpsOutputRect.xy + id.xy] = color;
}
