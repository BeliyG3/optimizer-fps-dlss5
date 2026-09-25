// Native pixel -> pixel of a guide texture through its sub-rect.
// Catmull-Rom resampling with five bilinear taps (the corner taps dropped, as in Jimenez's TAA): the
// residual is fine detail, and a bilinear fetch at a fractional position blurs it by an amount that
// changes every frame (camera zoom, walking, breathing in a cutscene) - detail that flickers between
// the full pass and the interpolated frames. Catmull-Rom keeps it.
float4 PwTSampleCatmullRom(Texture2D<float4> tex, float2 pixel, float2 texSize, float2 invSize)
{
    float2 samplePos = pixel;
    float2 texPos1 = floor(samplePos - 0.5) + 0.5;
    float2 f = samplePos - texPos1;
    float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    float2 w3 = f * f * (-0.5 + 0.5 * f);
    float2 w12 = w1 + w2;
    float2 offset12 = w2 / w12;
    float2 texPos0 = (texPos1 - 1.0) * invSize;
    float2 texPos3 = (texPos1 + 2.0) * invSize;
    float2 texPos12 = (texPos1 + offset12) * invSize;
    float4 result = 0.0;
    result += tex.SampleLevel(PwTLinearClamp, float2(texPos12.x, texPos0.y), 0.0) * w12.x * w0.y;
    result += tex.SampleLevel(PwTLinearClamp, float2(texPos0.x, texPos12.y), 0.0) * w0.x * w12.y;
    result += tex.SampleLevel(PwTLinearClamp, float2(texPos12.x, texPos12.y), 0.0) * w12.x * w12.y;
    result += tex.SampleLevel(PwTLinearClamp, float2(texPos3.x, texPos12.y), 0.0) * w3.x * w12.y;
    result += tex.SampleLevel(PwTLinearClamp, float2(texPos12.x, texPos3.y), 0.0) * w12.x * w3.y;
    // The five taps' weights do not sum to one (the corners are dropped): renormalise.
    float norm = w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y;
    result /= max(norm, 1e-4);
    // No ringing: the negative lobes overshoot next to a step, and in a linear HDR frame the step
    // beside a lamp is tens of units - the overshoot came out as black dots around every light (007
    // First Light). The result stays within the four texels it lies between.
    int2 corner = int2(texPos1 - 0.5);
    int2 last = int2(texSize) - 1;
    float4 a = tex.Load(int3(clamp(corner, int2(0, 0), last), 0));
    float4 b = tex.Load(int3(clamp(corner + int2(1, 0), int2(0, 0), last), 0));
    float4 c = tex.Load(int3(clamp(corner + int2(0, 1), int2(0, 0), last), 0));
    float4 d = tex.Load(int3(clamp(corner + int2(1, 1), int2(0, 0), last), 0));
    return clamp(result, min(min(a, b), min(c, d)), max(max(a, b), max(c, d)));
}

float2 PwTToRect(float2 nativePixel, float4 rect)
{
    return rect.xy + nativePixel * PwTNative.zw * rect.zw;
}

// A colour read can be full-frame while the other guides occupy different
// sub-rects. Decide the bilinear guard for the resource actually in use.
#ifdef PW_T_MODEL_GRID
bool PwTNeedsRectClamp(float4 rect)
{
    uint depthWidth, depthHeight;
    PwTDepth.GetDimensions(depthWidth, depthHeight);
    const bool colorPartial = all(rect == PwTColorRect) &&
        (any(PwTColorRect.xy != 0.0) || any(PwTColorRect.zw != PwTNative.xy));
    const bool depthPartial = all(rect == PwTDepthRect) &&
        (any(PwTDepthRect.xy != 0.0) || any(PwTDepthRect.zw != float2(depthWidth, depthHeight)));
    const bool motionPartial = all(rect == PwTMotionRect) &&
        (any(PwTMotionRect.xy != 0.0) || any(PwTMotionRect.zw != PwTMotionTex.xy));
    return colorPartial || depthPartial || motionPartial;
}
#endif

// The same as a texel to Load, kept inside the guide's own sub-rect.
//
// The callers reach past the frame as a matter of course: the end of a motion chain, a ring tap around a
// rejected pixel, a texel of the motion texture that lies outside the host's sub-rect. Load past the edge
// of a texture returns zeros, and a zero depth is not "no sample" but a real one - the camera plane in a
// 0..1 buffer, infinity in a reversed one, the origin in a linear view-space one - so the tests read a
// surface that is not there and either accept a stranger's contribution or throw the pixel's own away,
// in a band along the frame's edge. Where the guides occupy a sub-rect of a larger texture (a host that
// renders below the output size, an atlas) the same read lands in the neighbouring region instead, which
// is another frame's data. Clamping gives the rect's edge texel, as every sampler here already does.
int2 PwTRectTexel(float2 nativePixel, float4 rect)
{
    return int2(clamp(PwTToRect(nativePixel, rect), rect.xy, rect.xy + rect.zw - 1.0));
}

// A sub-rect needs a half-texel guard; on full-frame reads the sampler's edge
// clamp preserves long motion chains without a second pre-clamp.
float2 PwTRectUv(float2 nativePixel, float4 rect, float2 texSize)
{
    float2 inside = PwTToRect(nativePixel, rect);
#ifdef PW_T_MODEL_GRID
    if (PwTNeedsRectClamp(rect))
        return clamp(inside, rect.xy + 0.5, rect.xy + rect.zw - 0.5) / texSize;
    return inside / texSize;
#else
    return clamp(inside, rect.xy + 0.5, rect.xy + rect.zw - 0.5) / texSize;
#endif
}

// The accumulated displacement at a position given in motion-texture pixels, bilinear and likewise held
// inside the motion sub-rect.
float2 PwTSampleAccAt(float2 mvPixel)
{
    float2 inside = clamp(mvPixel, PwTMotionRect.xy + 0.5, PwTMotionRect.xy + PwTMotionRect.zw - 0.5);
    return PwTAccPrev.SampleLevel(PwTLinearClamp, inside / PwTMotionTex.xy, 0.0);
}

// How far apart two depth samples are, as a fraction, whatever the depth buffer's encoding. The plain
// relative difference |a - b| / max(a, b) reads distance ratios from a linear or a reversed-Z buffer
// (d ~ near / distance), but a conventional 0..1 buffer keeps everything past a few metres within a few
// percent of 1: a character at 2 m and the wall at 6 m differ by 3 %, under any usable tolerance, and the
// character's contribution was carried onto the wall it had uncovered (007 First Light, a translucent
// detailed ghost beside the head). There the distance ratio lives in 1 - d, so both are measured and the
// larger one counts. The second is only meaningful for samples inside 0..1.
float PwTDepthMismatch(float a, float b)
{
    if (!isfinite(a) || !isfinite(b)) return 1e9;
    float mismatch = abs(a - b) / max(max(abs(a), abs(b)), 1e-6);
    if (a >= 0.0 && a <= 1.0 && b >= 0.0 && b <= 1.0)
        mismatch = max(mismatch, abs(a - b) / max(max(1.0 - a, 1.0 - b), 1e-6));
    return mismatch;
}

// Mismatch between a depth value and the residual frame's depth AROUND a native position: the best of
// the four guide texels the position lies between. The guides are render resolution under a native
// frame and are read by nearest texel, and the upscaler jitters them: on the boundary of two surfaces a
// texel belongs to one surface this frame and to the other the next. One texel against one texel
// therefore fails in a lattice along every edge, and differently each frame - dotted dark outlines
// around lamps and edges that clicked while nothing moved (007 First Light). If any of the four texels
// shows this pixel's surface, the residual's frame did have it here.
float PwTDepthMismatchAround(float depthNow, float2 nativePixel)
{
    if (!isfinite(depthNow)) return 1e9; // PwTExpectedDepth: the surface was not in the residual's frame
    float2 g = PwTToRect(nativePixel, PwTDepthRect) - 0.5;
    int2 base = int2(floor(g));
    int2 lo = int2(PwTDepthRect.xy);
    int2 hi = int2(PwTDepthRect.xy + PwTDepthRect.zw) - 1;
    float best = 1e9;
    [unroll] for (int k = 0; k < 4; ++k)
    {
        int2 texel = clamp(base + int2(k & 1, k >> 1), lo, hi);
        best = min(best, PwTDepthMismatch(depthNow, PwTDepthF.Load(int3(texel, 0))));
    }
    return best;
}

// The depth to look for in the residual's frame for the surface at a native pixel of this frame.
//
// This frame's own depth is the wrong thing to compare once the camera travels: flying through a hall at
// 7 m/s a floor two metres ahead comes 6 % closer every frame, so four frames after a pass the same
// surface no longer matched its own depth within any tolerance that still tells a character from the
// wall behind him, and its contribution was thrown away over most of the picture - the longer since the
// pass the more, then all back at once (bench12, flying forward: the whole frame pulsed every N frames).
// PSExpect carries the depth the surface HAD in the residual's frame along with the chain, each link
// checked against the frame before it, where a frame's worth of travel is small; the tests compare
// that. Without the expectation (the pixel path, or switched off) it is this frame's depth as before.
float PwTExpectedDepth(float depthNow, float2 nativePixel)
{
#ifdef PW_T_EXPECT
    float4 expected = PwTExpect.Load(int3(PwTRectTexel(nativePixel, PwTMotionRect), 0));
    if (expected.w < 0.5) return expected.x;
#endif
    return depthNow;
}

// Residual of a full pass.
// Colour consistency between this frame's colour c and the residual frame's colour cf at the reprojected
// point, as a weight 1 (same thing) .. 0 (something else). Chromaticity first: a change of lighting
// (a turning face, a moving light) changes the luma of a surface but hardly its hue, while a
// disocclusion (teeth behind lips, red against white; skin against hair) changes the hue. Luma is
// tested too, with twice the tolerance, for same-hue disocclusions and moving shadows.
float PwTColourGate(float3 c, float3 cf, float tol)
{
    // The callers hold frames as they were read, i.e. encoded when the ratio domain is on. The test is
    // about what the surfaces look like, and a logarithm flattens exactly that: a red phone box and grey
    // concrete came out alike, and the box's contribution was carried onto the wall in a row of red
    // echoes. Compare in linear light.
    c = PwTDecodeFrame(c);
    cf = PwTDecodeFrame(cf);
    // The same floor as the luma term below: a dark pixel has no chroma to speak of, and under path
    // tracing its noise swung the normalised colour far enough to close the gate every other frame (a
    // dark cabinet that flickered while nothing moved).
    float3 nc = c / (dot(c, 1.0) + 0.06), nf = cf / (dot(cf, 1.0) + 0.06);
    float chroma = dot(abs(nc - nf), 1.0);                       // 0..2
    float lc = dot(c, float3(0.2126, 0.7152, 0.0722)), lf = dot(cf, float3(0.2126, 0.7152, 0.0722));
    float relL = abs(lc - lf) / (max(lc, lf) + 0.02);
    return (1.0 - smoothstep(0.75 * tol, 1.5 * tol, chroma)) * (1.0 - smoothstep(2.0 * tol, 4.0 * tol, relL));
}
