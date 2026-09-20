// Temporal NR modes (PeripheralWarp stage 24): three full-screen passes that let the host's
// Neural Rendering model run on every N-th frame while the other frames receive the model's last
// residual, reprojected along the host's motion vectors.
//
//   PSResidual    R = nrOut - color                       (native, RGBA16F)
//   PSAccumulate  acc = mv*scale + accPrev(p + mv*scale)  (motion-texture space, RG16F)
//   PSDownsample  Rlow = box-filtered R (1/16 per axis): the fill for pixels whose reprojection is rejected
//   PSReproject   out = color + lerp(Rlow(p), R(p + acc), w)
//
// Motion vectors point from the current frame to the previous one (NGX convention); "acc" is the
// displacement from the current frame back to the frame whose residual is stored, in pixels of the
// host's motion texture. Nothing here depends on the warp layout.
#include "fullscreen.hlsli"

cbuffer PwTemporalConstants : register(b0)
{
    // 9 float4 (36 root constants). Every lane below is read by at least one pass; two lanes of
    // PwTSmooth are padding to the float4 boundary.
    float4 PwTNative;      // native width, height, 1/width, 1/height
    float4 PwTColorRect;   // host colour sub-rect x, y, w, h (colour texture pixels)
    float4 PwTMotionRect;  // host motion sub-rect x, y, w, h (motion texture pixels)
    float4 PwTDepthRect;   // host depth sub-rect x, y, w, h (depth texture pixels)
    float4 PwTMotionTex;   // motion texture width, height, MVecScale X, MVecScale Y
    // x: accumulate only - validate each link of the chain against the residual's frame (0/1)
    // y: accumulate - the previous accumulation is valid (0/1) | reproject - Catmull-Rom residual filter (0/1)
    // z: debug visualisation (0 = off; reproject only)
    // w: relative depth mismatch that counts as a surface change (the disocclusion tolerance)
    float4 PwTParams;
    float4 PwTTune;        // colour tolerance (relative luma, 0 = off), motion sign (+1/-1), raw interpolation (>0.5), disable history neighbour search (<-0.5), residual blend weight (Residual); fill floor toggle 0/1 (Reproject)
    float4 PwTFill;        // hole fill with the low-res residual (0/1), low-res width, low-res height, depth-guided chain fetch (0 off, 1 four texels, >1 = search radius in native px)
    float4 PwTSmooth;      // acceptance tap radius scale (0 = 1), guided smoothing radius in px (26.6.X, 0 = off), ratio-domain scale s (0 = frames as they are; see PwTEncodeFrame), share of the NEW residual while it is phased in (PW_T_RAMP; 0 or 1 = all of it)
};

Texture2D<float4> PwTColor : register(t0);     // host colour (native)
Texture2D<float4> PwTFresh : register(t1);     // the model's output of this frame (native): residual source / fresh centre
Texture2D<float4> PwTResidual : register(t2);  // stored residual (native)
Texture2D<float2> PwTMotion : register(t3);    // host motion vectors
Texture2D<float2> PwTAccPrev : register(t4);   // accumulated displacement so far (motion-texture space)
Texture2D<float>  PwTDepth : register(t5);     // host depth of this frame
Texture2D<float>  PwTDepthF : register(t6);    // host depth of the residual's frame
Texture2D<float4> PwTColorF : register(t7);    // host colour of the residual's frame
Texture2D<float4> PwTResidualLow : register(t8); // box-filtered residual (native / 16 per axis)
#ifdef PW_T_EXPECT
// The depth each pixel's surface had in the residual's frame, carried along the chain (PSExpect).
// Motion-texture size. x: expected depth, quiet NaN when unknown. w: 1 when expectation is off.
// Every finite depth (including negative linear view-space Z) is a real sample, never a marker.
// y: this frame's link held (1) - the surface was in the previous frame where the vector points - or not (0).
// z: the same depth as geometry - where the surface was hidden, lent by the neighbours at its depth, so an
//    uncovered pixel still knows how deep it lay in the residual's frame (PwTFromOlderPasses).
Texture2D<float4> PwTExpect : register(t11);
#endif
#ifdef PW_T_HISTORY
// The two full passes BEFORE the last one ([0] the nearer). Wrappers keep pictures at 1/3 per axis
// and geometry at 1/6 (about 38 MiB at 4K). Per pass, in ITS
// frame: the residual, the frame's colour and depth; and in the frame of the pass AFTER it: the link -
// xy the chain from that later frame back to this one (motion-texture pixels), z the depth the surface had
// in this one; w is the source depth at the SAME sampled link position. See PSHistory and PwTFromOlderPasses.
Texture2D<float4> PwTHistResidual[2] : register(t12);
Texture2D<float4> PwTHistColour[2] : register(t14);
Texture2D<float>  PwTHistDepth[2] : register(t16);
Texture2D<float4> PwTHistLink[2] : register(t18);
#endif
#ifdef PW_T_RAMP
// The previous pass's residual as it stood on the frame of the new pass: moved there along the chain,
// and equal to the new residual wherever the move failed its tests. See PSResidualOld.
Texture2D<float4> PwTResidualOld : register(t10);
#endif
SamplerState PwTLinearClamp : register(s0);
SamplerState PwTPointClamp : register(s1);

// Frames in a ratio domain (PwTSmooth.z = s > 0; 0 = off, the frames are used as they are).
//
// The residual is a DIFFERENCE, and that only behaves on display-referred frames. On a linear HDR
// frame the model's contribution beside a lamp is tens of units; carried a fraction of a pixel off (a
// third-person camera sways even when the player stands still) it lands on a neighbour worth 0.5 and
// the sum goes black - dark streaks along every lamp and high-contrast edge (007 First Light). With the
// frames read as log(1 + x / s) the residual is a log ratio: the same misalignment makes a dark pixel
// somewhat darker or brighter, never black, and a lamp somewhat brighter, which clips to white anyway.
// Everything between the reads and the final write works on the encoded values; whoever writes the
// frame decodes with PwTDecodeFrame (the compute entry points do; the pixel path leaves s at 0).
float3 PwTEncodeFrame(float3 linearColour)
{
    return PwTSmooth.z > 0.0 ? log(1.0 + max(linearColour, 0.0) / PwTSmooth.z) : linearColour;
}

float3 PwTDecodeFrame(float3 encoded)
{
    return PwTSmooth.z > 0.0 ? PwTSmooth.z * (exp(min(encoded, 40.0)) - 1.0) : encoded;
}

float4 PwTLoadFrame(Texture2D<float4> frame, int3 location)
{
    float4 value = frame.Load(location);
    return float4(PwTEncodeFrame(value.rgb), value.a);
}

PwFullscreenVertex VSMain(uint vertexId : SV_VertexID) { return PwFullscreenVS(vertexId); }


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

// And as a normalised coordinate for a bilinear read, half a texel in from the rect's edge: the clamp
// sampler knows the whole texture, not the part of it this frame lives in.
float2 PwTRectUv(float2 nativePixel, float4 rect, float2 texSize)
{
    float2 inside = clamp(PwTToRect(nativePixel, rect), rect.xy + 0.5, rect.xy + rect.zw - 0.5);
    return inside / texSize;
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

// Residual of a full pass. With PwTTune.w > 0 the previous residual (t2), moved along t4 = the
// displacement from this pass's frame back to the previous pass's frame (motion-texture pixels,
// already scaled: the accumulation chain in the sync mode, the kick's chain copy in the background
// mode), is blended in where the depth still matches (t5 now vs t6 then). The model's output differs
// slightly from pass to pass even in a still scene, and with N passes apart the fine detail (teeth,
// nostrils, lips) snapped from one version to the next; the blend turns the snap into a fade. The
// weight is fixed in the hook; disocclusions get the new residual alone.
float4 PSResidual(PwFullscreenVertex input) : SV_Target0
{
    int2 p = int2(input.position.xy);
    int2 c = PwTRectTexel(input.position.xy, PwTColorRect);
    float4 fresh = PwTLoadFrame(PwTFresh, int3(p, 0));
    float4 color = PwTLoadFrame(PwTColor, int3(c, 0));
    float3 r = fresh.rgb - color.rgb;
    if (PwTTune.w > 0.0)
    {
        float2 mvPixel = PwTToRect(input.position.xy, PwTMotionRect);
        float2 acc = PwTSampleAccAt(mvPixel);
        float2 q = input.position.xy + acc * PwTNative.xy / PwTMotionRect.zw;
        if (all(isfinite(q)) && all(q >= 0.0) && all(q < PwTNative.xy))
        {
            float dc = PwTDepth.Load(int3(PwTRectTexel(input.position.xy, PwTDepthRect), 0));
            float wd = 1.0 - smoothstep(PwTParams.w, 2.0 * PwTParams.w, PwTDepthMismatchAround(PwTExpectedDepth(dc, input.position.xy), q));
            // Adaptive: full weight where the pixel barely moved between the passes (a face in a cutscene:
            // the model's pass-to-pass jitter of fine detail is what flickers there) and none where it
            // moved more than a few pixels or the colour under it changed (lips, a turning head, walking),
            // so nothing lags or trails.
            float2 d = q - input.position.xy;
            float motionFade = 1.0 - smoothstep(2.0, 6.0, length(d));
            float colourGate = 1.0;
            if (PwTTune.x > 0.0)
            {
                float3 cf = PwTLoadFrame(PwTColorF, int3(PwTRectTexel(q, PwTColorRect), 0)).rgb;
                colourGate = PwTColourGate(color.rgb, cf, 0.5 * PwTTune.x); // half the reprojection's tolerance
            }
            // The previous residual, bilinear (no ringing), clamped to the range of the new residual in the
            // 3x3 neighbourhood (TAA-style): an edge of another surface (hair over a cheek, a collar at
            // the neck) that the vectors carried here cannot introduce a value the new pass does not have
            // nearby - no dark spots or bright lines, only the jitter of matching detail is averaged.
            float3 old = PwTResidual.SampleLevel(PwTLinearClamp, q * PwTNative.zw, 0.0).rgb;
            float3 mn = r, mx = r;
            [unroll] for (int oy = -1; oy <= 1; ++oy)
                [unroll] for (int ox = -1; ox <= 1; ++ox)
                {
                    if (ox == 0 && oy == 0) continue;
                    int2 pn = clamp(p + int2(ox, oy), int2(0, 0), int2(PwTNative.xy) - 1);
                    float3 rn = PwTLoadFrame(PwTFresh, int3(pn, 0)).rgb - PwTLoadFrame(PwTColor, int3(PwTRectTexel(float2(pn) + 0.5, PwTColorRect), 0)).rgb;
                    mn = min(mn, rn); mx = max(mx, rn);
                }
            old = clamp(old, mn, mx);
            if (all(isfinite(old))) r = lerp(r, old, PwTTune.w * wd * motionFade * colourGate);
        }
    }
    if (any(!isfinite(r))) r = fresh.rgb - color.rgb;
    return float4(r, 0.0);
}

#ifdef PW_T_RAMP
// The previous pass's residual on the NEW pass's frame, for phasing the new pass in.
//
// In motion the model's answer N frames apart differs over most of the picture (its own history is N
// frames old, the carried detail has drifted), and replacing one by the other in a single frame is a
// click across the whole screen every N frames - flying through a level made the picture pulse. Shown
// for a few frames as a mix that slides from the old residual to the new one, the same change is a
// short cross-fade. The old residual is moved here once, along the chain that led back to its frame
// (t4; its snapshots t6/t7 are still in place), and wherever that move fails the depth or colour test
// the new residual stands in, so nothing is faded in from a wrong surface. t1 = the new residual,
// t2 = the old one.
float4 PSResidualOld(PwFullscreenVertex input) : SV_Target0
{
    int2 p = int2(input.position.xy);
    float3 newer = PwTFresh.Load(int3(p, 0)).rgb;
    float2 mvPixel = PwTToRect(input.position.xy, PwTMotionRect);
    float2 acc = PwTSampleAccAt(mvPixel);
    float2 q = input.position.xy + acc * PwTNative.xy / PwTMotionRect.zw;
    if (any(!isfinite(q)) || any(q < 0.0) || any(q >= PwTNative.xy)) return float4(newer, 0.0);
    float dc = PwTDepth.Load(int3(PwTRectTexel(input.position.xy, PwTDepthRect), 0));
    float keep = 1.0 - smoothstep(PwTParams.w, 2.0 * PwTParams.w, PwTDepthMismatchAround(PwTExpectedDepth(dc, input.position.xy), q));
    if (PwTTune.x > 0.0)
    {
        float3 c = PwTLoadFrame(PwTColor, int3(PwTRectTexel(input.position.xy, PwTColorRect), 0)).rgb;
        float3 cf = PwTLoadFrame(PwTColorF, int3(PwTRectTexel(q, PwTColorRect), 0)).rgb;
        keep *= PwTColourGate(c, cf, PwTTune.x);
    }
    float3 older = PwTSampleCatmullRom(PwTResidual, q, PwTNative.xy, PwTNative.zw).rgb;
    if (any(!isfinite(older))) return float4(newer, 0.0);
    return float4(lerp(newer, older, keep), 0.0);
}
#endif

// Runs over the low-res residual: each texel averages a block of the residual (16 bilinear taps
// spread over the block), so rejected pixels can be filled with the model's local tone instead of
// nothing (a hole without the residual is 5-8 % darker than its surroundings in tone-mapped hosts).
// The source is the stored residual (t2).
float4 PSDownsample(PwFullscreenVertex input) : SV_Target0
{
    float2 block = PwTNative.xy / PwTFill.yz;            // native pixels per low-res texel
    float2 origin = (input.position.xy - 0.5) * block;      // native position of the block's corner
    float3 sum = 0.0;
    [unroll] for (int y = 0; y < 4; ++y)
        [unroll] for (int x = 0; x < 4; ++x)
        {
            float2 p = origin + (float2(x, y) + 0.5) * block * 0.25;
            sum += PwTResidual.SampleLevel(PwTLinearClamp, p * PwTNative.zw, 0.0).rgb;
        }
    return float4(sum / 16.0, 0.0);
}

// Runs over the whole motion texture. Output in motion-texture pixels (already scaled).
float2 PSAccumulate(PwFullscreenVertex input) : SV_Target0
{
    int2 q = int2(input.position.xy);
    float2 mv = PwTMotion.Load(int3(q, 0)) * PwTMotionTex.zw * PwTTune.y;
    if (any(!isfinite(mv))) mv = float2(0.0, 0.0); // a NaN vector would poison the chain until the next full pass
    float2 acc = mv;
    if (PwTParams.y > 0.5)
    {
        float2 prev = input.position.xy + mv;
        float2 nativeHere = (input.position.xy - PwTMotionRect.xy) / PwTMotionRect.zw * PwTNative.xy;
        float depthHere = PwTDepth.Load(int3(PwTRectTexel(nativeHere, PwTDepthRect), 0));
        acc += PwTSampleAccAt(prev);
        if (any(!isfinite(acc))) acc = mv;
        // A surface the residual's frame did not show has nothing to be validated against; the reprojection
        // rejects it by its unknown depth. (Lending such a pixel the chain of its neighbours, so that it
        // knows where it lay under the occluder, was tried: the older passes' lookup finds its way from the
        // neighbours' links anyway, and in forward flight the lent chains cost 2 % of the whole frame.)
        if (PwTParams.x > 0.5 && isfinite(PwTExpectedDepth(depthHere, nativeHere)))
        {
            // Validate the link (main chain only): the chain is inherited through occluders - a pixel the
            // lower lip just uncovered takes over the lip's accumulated displacement and lands on the
            // lip in the residual's frame; every frame of the opening mouth added another copy of the lip
            // line across the teeth (26.6.Q, cutegirl bench). If the residual's frame does not show this
            // pixel's surface at the chain's end (depth or colour), start the chain afresh from this
            // frame's vector: the reprojection then rejects it honestly and fills locally.
            float2 nativePixel = (input.position.xy - PwTMotionRect.xy) / PwTMotionRect.zw * PwTNative.xy;
            float2 qN = nativePixel + acc * PwTNative.xy / PwTMotionRect.zw;
            bool inside = all(qN >= 0.0) && all(qN < PwTNative.xy);
            if (inside)
            {
                float dc = PwTDepth.Load(int3(PwTRectTexel(nativePixel, PwTDepthRect), 0));
                bool ok = PwTDepthMismatchAround(PwTExpectedDepth(dc, nativePixel), qN) <= 2.0 * PwTParams.w;
                if (ok && PwTTune.x > 0.0)
                {
                    float3 c = PwTLoadFrame(PwTColor, int3(PwTRectTexel(nativePixel, PwTColorRect), 0)).rgb;
                    float3 cf = PwTLoadFrame(PwTColorF, int3(PwTRectTexel(qN, PwTColorRect), 0)).rgb;
                    ok = PwTColourGate(c, cf, PwTTune.x) > 0.05;
                }
                if (!ok) acc = mv;
            }
        }
    }
    return acc;
}

// The chain as the MODEL's motion vectors on a full pass after carried frames (motion-texture size).
//
// The model keeps a history of its own, N frames old by now, and finds it through these vectors. Where
// the chain holds, that is the chain. Where it does not - a pixel uncovered since the model last looked:
// the accumulation restarted its chain from one frame's vector - the vector pointed the model at whatever
// stood there N frames ago, the occluder, and the model blended its old picture of him into the wall he
// had left: a translucent head and collar beside the character, a pale outline of the head on a lit wall
// (007 First Light). That copy is part of the model's answer, so it entered the residual with a matching
// colour on both sides and no test downstream could tell. A pixel whose chain does not end on its own
// surface in the residual's frame gets a vector that leaves the picture instead: no history there, the
// model treats it as it treats any disocclusion of its own.
float2 PSModelMotion(PwFullscreenVertex input) : SV_Target0
{
    float2 acc = PwTAccPrev.Load(int3(int2(input.position.xy), 0));
    const float2 none = float2(4.0 * PwTMotionTex.x, 0.0);
    if (any(!isfinite(acc))) return none;
    float2 nativePixel = (input.position.xy - PwTMotionRect.xy) / PwTMotionRect.zw * PwTNative.xy;
    float2 qN = nativePixel + acc * PwTNative.xy / PwTMotionRect.zw;
    if (any(qN < 0.0) || any(qN >= PwTNative.xy)) return none;
    float dc = PwTDepth.Load(int3(PwTRectTexel(nativePixel, PwTDepthRect), 0));
    if (PwTDepthMismatchAround(PwTExpectedDepth(dc, nativePixel), qN) > 2.0 * PwTParams.w) return none;
    if (PwTTune.x > 0.0)
    {
        float3 c = PwTLoadFrame(PwTColor, int3(PwTRectTexel(nativePixel, PwTColorRect), 0)).rgb;
        float3 cf = PwTLoadFrame(PwTColorF, int3(PwTRectTexel(qN, PwTColorRect), 0)).rgb;
        if (PwTColourGate(c, cf, PwTTune.x) <= 0.05) return none;
    }
    return acc;
}

#ifdef PW_T_EXPECT
// One link of the expectation (see PwTExpectedDepth), over the motion texture, BEFORE this frame's
// accumulation. t6 = the PREVIOUS frame's depth, t11 = the expectation as of the previous frame.
// PwTParams.x: 1 on, 0 off (writes w=1). PwTParams.y: 0 on the first frame after a pass - the previous
// frame IS the residual's frame and its depth is the expectation.
float4 PSExpect(PwFullscreenVertex input) : SV_Target0
{
    float2 toNative = PwTNative.xy / PwTMotionRect.zw;
    float2 nativePixel = (input.position.xy - PwTMotionRect.xy) * toNative;
    float dc = PwTDepth.Load(int3(PwTRectTexel(nativePixel, PwTDepthRect), 0));
    if (PwTParams.x < 0.5) return float4(dc, 1.0, dc, 1.0);
    float2 mv = PwTMotion.Load(int3(int2(input.position.xy), 0)) * PwTMotionTex.zw * PwTTune.y;
    if (any(!isfinite(mv))) return float4(asfloat(0x7fc00000u), 0.0, dc, 0.0);
    float2 before = nativePixel + mv * toNative;
    if (any(before < 0.0) || any(before >= PwTNative.xy)) return float4(asfloat(0x7fc00000u), 0.0, dc, 0.0);
    // The previous frame's texel this surface was in: the best of the four around the vector's end
    // (render-resolution, jittered guides: see PwTDepthMismatchAround).
    float2 g = PwTToRect(before, PwTDepthRect) - 0.5;
    int2 base = int2(floor(g));
    int2 lo = int2(PwTDepthRect.xy);
    int2 hi = int2(PwTDepthRect.xy + PwTDepthRect.zw) - 1;
    float best = 1e9;
    int2 bestTexel = clamp(base, lo, hi);
    float bestDepth = 0.0;
    [unroll] for (int k = 0; k < 4; ++k)
    {
        int2 texel = clamp(base + int2(k & 1, k >> 1), lo, hi);
        float d = PwTDepthF.Load(int3(texel, 0));
        float m = PwTDepthMismatch(dc, d);
        if (m < best) { best = m; bestTexel = texel; bestDepth = d; }
    }
    // One frame of travel, not N: three tolerances cover a fast camera and still part a character
    // from the wall behind him.
    if (best <= 3.0 * PwTParams.w)
    {
        if (PwTParams.y < 0.5) return float4(bestDepth, 1.0, bestDepth, 0.0);
        float2 there = (float2(bestTexel) + 0.5 - PwTDepthRect.xy) / PwTDepthRect.zw * PwTNative.xy;
        float4 was = PwTExpect.Load(int3(PwTRectTexel(there, PwTMotionRect), 0));
        return float4(was.w > 0.5 ? asfloat(0x7fc00000u) : was.x, 1.0, !isfinite(was.z) ? dc : was.z, 0.0);
    }
    // Hidden in the previous frame: what is known about it (x) ends here, but how deep it lay in the
    // residual's frame (z) is geometry, and the neighbours of the previous frame at this depth lend it.
    float2 beforeG = PwTToRect(before, PwTDepthRect);
    [loop] for (int ring = 1; ring <= 4; ++ring)
    {
        float r = (float) (1 << ring); // 2, 4, 8, 16 guide texels
        float sum = 0.0, n = 0.0;
        [loop] for (int j = 0; j < 8; ++j)
        {
            float ang = (float) j * 0.7853982;
            int2 texel = clamp(int2(floor(beforeG + float2(cos(ang), sin(ang)) * r)), lo, hi);
            float d = PwTDepthF.Load(int3(texel, 0));
            if (PwTDepthMismatch(dc, d) > 3.0 * PwTParams.w) continue;
            float2 there = (float2(texel) + 0.5 - PwTDepthRect.xy) / PwTDepthRect.zw * PwTNative.xy;
            float z = PwTParams.y < 0.5 ? d : PwTExpect.Load(int3(PwTRectTexel(there, PwTMotionRect), 0)).z;
            if (!isfinite(z)) continue;
            sum += z; n += 1.0;
        }
        if (n > 0.5) return float4(asfloat(0x7fc00000u), 0.0, sum / n, 0.0);
    }
    return float4(asfloat(0x7fc00000u), 0.0, dc, 0.0);
}
#endif

// Registration of the chain against the real frames (runs after PSAccumulate, over the same texels).
//
// The chain is a sum of per-frame vectors, each read between texels of a render-resolution field: every
// link adds a fraction of a pixel, and moving towards an object all links err the same way. Nothing in
// the vectors can correct that, but the frames can: the residual's frame (t7) and this frame (t0) are
// both real. One Lucas-Kanade step finds the shift d that best explains this frame's neighbourhood by
// the residual frame's around the chain's end:  sum g g^T . d = sum g (I_now - I_then),  g = grad I_then,
// over a 3x3 window of samples 3 px apart, on the luminance of the frames as read (a log under the ratio
// domain, so the fit is indifferent to exposure). The correction goes back INTO the chain, so it holds
// for every later frame and each step only ever has the last frame's error to remove - well within what
// a single step can see. Flat or one-directional neighbourhoods are left alone (damped solve, a floor on
// the smaller eigenvalue), disocclusions too, and a step is limited to a pixel.
float PwTLumaThen(float2 nativePixel)
{
    float width, height;
    PwTColorF.GetDimensions(width, height);
    float2 uv = PwTRectUv(nativePixel, PwTColorRect, float2(width, height));
    return dot(PwTEncodeFrame(PwTColorF.SampleLevel(PwTLinearClamp, uv, 0.0).rgb), float3(0.2126, 0.7152, 0.0722));
}

// This frame read the same way as the other one: both at a continuous position, bilinearly. Reading this
// one by whole pixel against a fractional position in the other shows up as a shift of up to half a pixel
// that is not there, and a still scene started to drift.
float PwTLumaNow(float2 nativePixel)
{
    float width, height;
    PwTColor.GetDimensions(width, height);
    float2 uv = PwTRectUv(nativePixel, PwTColorRect, float2(width, height));
    return dot(PwTEncodeFrame(PwTColor.SampleLevel(PwTLinearClamp, uv, 0.0).rgb), float3(0.2126, 0.7152, 0.0722));
}

float2 PSRefine(PwFullscreenVertex input) : SV_Target0
{
    float2 acc = PwTAccPrev.Load(int3(int2(input.position.xy), 0));
    if (any(!isfinite(acc))) return float2(0.0, 0.0);
    float2 toNative = PwTNative.xy / PwTMotionRect.zw;
    float2 p = (input.position.xy - PwTMotionRect.xy) * toNative;
    float2 q = p + acc * toNative;
    const float margin = 8.0;
    if (any(p < margin) || any(p >= PwTNative.xy - margin) || any(q < margin) || any(q >= PwTNative.xy - margin)) return acc;
    // Another surface at the chain's end: nothing to register against.
    float dc = PwTDepth.Load(int3(PwTRectTexel(p, PwTDepthRect), 0));
    if (PwTDepthMismatchAround(PwTExpectedDepth(dc, p), q) > 2.0 * PwTParams.w) return acc;

    float axx = 0.0, axy = 0.0, ayy = 0.0, bx = 0.0, by = 0.0;
    [unroll] for (int oy = -1; oy <= 1; ++oy)
        [unroll] for (int ox = -1; ox <= 1; ++ox)
        {
            float2 o = float2(ox, oy) * 3.0;
            float now = PwTLumaNow(p + o);
            float2 s = q + o;
            float then = PwTLumaThen(s);
            float2 g = 0.5 * float2(PwTLumaThen(s + float2(1.0, 0.0)) - PwTLumaThen(s - float2(1.0, 0.0)),
                                    PwTLumaThen(s + float2(0.0, 1.0)) - PwTLumaThen(s - float2(0.0, 1.0)));
            float e = now - then;
            axx += g.x * g.x; axy += g.x * g.y; ayy += g.y * g.y;
            bx += g.x * e; by += g.y * e;
        }
    // Damped solve; the smaller eigenvalue says whether the window pins the shift in both directions.
    const float damping = 2.0e-3;
    axx += damping; ayy += damping;
    float det = axx * ayy - axy * axy;
    float trace = axx + ayy;
    float smaller = 0.5 * (trace - sqrt(max(trace * trace - 4.0 * det, 0.0)));
    if (smaller < 4.0 * damping || det <= 0.0) return acc;
    float2 d = float2(ayy * bx - axy * by, axx * by - axy * bx) / det;
    if (any(!isfinite(d))) return acc;
    float len = length(d);
    if (len < 0.35) return acc;               // under a third of a pixel it is the frame's noise, not a shift: a still scene must stay still
    d *= min(1.0, 1.0 / len) * 0.8;           // at most a pixel a step, slightly under-corrected: no overshoot on noise
    return acc + d / toNative;
}

// The accumulated displacement at a native pixel. The chain is stored at the motion texture's
// resolution; a plain bilinear fetch mixes the vectors of the two surfaces that meet at a depth edge,
// so the residual of one surface is dragged over the other (doubled edges of the far object next to a
// near one whenever they move differently - any camera translation). With PwTFill.w the four texels
// around the position are weighted only where their depth matches this pixel's depth (within the
// depth tolerance); if none matches, the closest in depth is taken.
float2 PwTFetchAcc(float2 p, float dc)
{
    float2 mvPixel = PwTToRect(p, PwTMotionRect);
    float2 result = PwTSampleAccAt(mvPixel);
    if (PwTFill.w < 0.5) return result;
    float2 f = mvPixel - 0.5;
    float2 base = floor(f);
    float2 t = f - base;
    float wsum = 0.0;
    float2 acc = float2(0.0, 0.0);
    float2 closest = float2(0.0, 0.0);
    float closestDiff = 1e30;
    [unroll] for (int k = 0; k < 4; ++k)
    {
        int2 o = int2(k & 1, k >> 1);
        int2 tex = clamp(int2(base) + o, int2(PwTMotionRect.xy), int2(PwTMotionRect.xy + PwTMotionRect.zw) - 1);
        float2 nativePixel = (float2(tex) + 0.5 - PwTMotionRect.xy) / PwTMotionRect.zw * PwTNative.xy;
        float dt = PwTDepth.Load(int3(PwTRectTexel(nativePixel, PwTDepthRect), 0));
        float diff = PwTDepthMismatch(dt, dc);
        float2 a = PwTAccPrev.Load(int3(tex, 0));
        float bw = (o.x == 0 ? 1.0 - t.x : t.x) * (o.y == 0 ? 1.0 - t.y : t.y);
        if (diff <= PwTParams.w) { acc += a * bw; wsum += bw; }
        if (diff < closestDiff) { closestDiff = diff; closest = a; }
    }
    // 26.14: none of the four texels belongs to this pixel's surface. With estimated (optical-flow) vectors
    // the texture is block-constant, so the near surface's vector covers a band of background around it and
    // the whole band reprojects onto the near object (silhouette-shaped halo). Search outward, up to
    // PwTFill.w native pixels (values > 1.5; 1 = the four texels only), for the nearest texel whose depth
    // matches this pixel and take its vector.
    if (wsum <= 1e-4 && PwTFill.w > 1.5)
    {
        float radius = PwTFill.w;
        float2 toMv = PwTMotionRect.zw / PwTNative.xy; // native px -> motion texels
        float bestDiff = closestDiff;
        [loop] for (int ring = 1; ring <= 4; ++ring)
        {
            float r = radius * (float) ring / 4.0;
            [unroll] for (int k = 0; k < 8; ++k)
            {
                float ang = (float) k * 0.7853982;
                float2 offs = float2(cos(ang), sin(ang)) * r;
                int2 tex = clamp(int2(floor(mvPixel + offs * toMv)), int2(PwTMotionRect.xy), int2(PwTMotionRect.xy + PwTMotionRect.zw) - 1);
                float2 nativePixel = (float2(tex) + 0.5 - PwTMotionRect.xy) / PwTMotionRect.zw * PwTNative.xy;
                float dt = PwTDepth.Load(int3(PwTRectTexel(nativePixel, PwTDepthRect), 0));
                float diff = PwTDepthMismatch(dt, dc);
                if (diff < bestDiff) { bestDiff = diff; closest = PwTAccPrev.Load(int3(tex, 0)); }
            }
            if (bestDiff <= PwTParams.w) break;
        }
    }
    result = (wsum > 1e-4) ? (acc / wsum) : closest;
    if (any(!isfinite(result))) result = float2(0.0, 0.0);
    return result;
}

// Two pixels of the frame look like the same surface, for a pixel that cannot go by its depth (rejected on
// a silhouette). PwTColourGate is built to forgive lighting and path-tracing noise and takes a dark wall
// for dark hair; here a plain relative difference in linear light, strict.
float PwTSameLook(float3 encodedA, float3 encodedB)
{
    float3 a = PwTDecodeFrame(encodedA), b = PwTDecodeFrame(encodedB);
    return 1.0 - smoothstep(0.08, 0.16, dot(abs(a - b), 1.0) / (dot(a + b, 1.0) + 0.02));
}

#ifdef PW_T_HISTORY
// A full pass on its way into the store, at the store's size (a sixth of the frame per axis).
// PwTParams.x = 0: the residual (t2) and the frame it was made on (t0), box-averaged.
// PwTParams.x = 1: that frame's depth (t6, the nearest texel - depths do not average), and the link FROM
//                  the pass that replaces it: the chain (t4) and the geometric depth (t11.z) as they stand on
//                  the new pass's frame, which is this one.
struct PwTHistoryOut { float4 first : SV_Target0; float4 second : SV_Target1; };
PwTHistoryOut PSHistory(PwFullscreenVertex input)
{
    PwTHistoryOut o = (PwTHistoryOut)0;
    float2 centre = input.uv * PwTNative.xy;
    if (PwTParams.x < 0.5)
    {
        float2 cell = PwTNative.xy / PwTFill.yz; // repurposed for this pass: the store's size
        float3 residual = 0.0, colour = 0.0;
        [unroll] for (int y = 0; y < 3; ++y)
            [unroll] for (int x = 0; x < 3; ++x)
            {
                float2 p = clamp(centre + (float2(x, y) - 1.0) * cell / 3.0, 0.5, PwTNative.xy - 0.5);
                residual += PwTResidual.SampleLevel(PwTLinearClamp, p * PwTNative.zw, 0.0).rgb;
                colour += PwTColor.Load(int3(PwTRectTexel(p, PwTColorRect), 0)).rgb;
            }
        o.first = float4(any(!isfinite(residual)) ? float3(0.0, 0.0, 0.0) : residual / 9.0, 0.0);
        o.second = float4(max(colour / 9.0, 0.0), 0.0);
    }
    else
    {
        float depth = PwTDepthF.Load(int3(PwTRectTexel(centre, PwTDepthRect), 0));
        int2 texel = PwTRectTexel(centre, PwTMotionRect);
        float2 chain = PwTAccPrev.Load(int3(texel, 0));
        float then = asfloat(0x7fc00000u);
#ifdef PW_T_EXPECT
        then = PwTExpect.Load(int3(texel, 0)).z;
#endif
        o.first = float4(depth, 0.0, 0.0, 0.0);
        // Keep invalid chains invalid; zero displacement would fabricate a valid static link.
        float sourceDepth = PwTDepth.Load(int3(PwTRectTexel(centre, PwTDepthRect), 0));
        o.second = float4(chain, then, sourceDepth);
    }
    return o;
}

// What an older full pass added to this surface, for a pixel the last pass cannot serve.
//
// The band a walker uncovers was behind him when the model last ran, but in front of the camera a pass
// or two before: the model's own contribution for it exists, only older. `q` is where the pixel was in
// the last pass's frame, as far as its chain knows, and
// `depthThere` how deep it lay there (the expectation's z). Each stored pass has the link that leads on
// from the frame after it; where the pixel stood behind something, the link is the occluder's, so it is
// taken from the positions around at the pixel's own depth, nearest first - the background beside him.
// A pass is taken if its frame shows this pixel's depth and look at the link's end; else the next one is
// tried from there. Returns the weight; `older` the residual.
float PwTFromOlderPasses(float2 q, float depthThere, float3 colourNow, int passes, out float3 older)
{
    older = float3(0.0, 0.0, 0.0);
    const float trust[2] = { 0.85, 0.7 }; // older is a little less the model's present opinion
    float2 at = q;
    float depthAt = depthThere;
    [unroll] for (int level = 0; level < 2; ++level)
    {
        if (level >= passes) return 0.0;
        uint w, h;
        PwTHistLink[level].GetDimensions(w, h);
        float2 toStore = float2(w, h) * PwTNative.zw;
        float3 link = float3(0.0, 0.0, 0.0);
        bool found = false;
        [loop] for (int ring = 0; ring <= (PwTTune.z < -0.5 ? 0 : 4) && !found; ++ring)
        {
            float r = ring == 0 ? 0.0 : 6.0 * (float) (1 << (ring - 1)); // 0, 6, 12, 24, 48 px
            float best = 3.0 * PwTParams.w;
            [loop] for (int k = 0; k < (ring == 0 ? 1 : 8); ++k)
            {
                float ang = (float) k * 0.7853982;
                float2 c = clamp(at + float2(cos(ang), sin(ang)) * r, 0.0, PwTNative.xy - 1.0);
                float4 l = PwTHistLink[level].Load(int3(clamp(int2(c * toStore), int2(0, 0), int2(w, h) - 1), 0));
                if (any(!isfinite(l))) continue;
                // The store is coarser than the guides. Test the source depth stored WITH this
                // link, not a guide at c that may belong to the other side of a silhouette.
                float mismatch = PwTDepthMismatch(depthAt, l.w);
                if (mismatch <= best)
                {
                    best = mismatch;
                    link = l.xyz;
                    found = true;
                }
            }
            // One actual link: averaging neighbours can synthesize a third object's motion/depth.
        }
        if (!found) return 0.0;
        at += link.xy * PwTNative.xy / PwTMotionRect.zw;
        depthAt = link.z;
        if (any(at < 0.0) || any(at >= PwTNative.xy)) return 0.0;
        // This pass's frame at the link's end: the best of the four stored depths around (the store is coarse).
        float2 g = at * toStore - 0.5;
        int2 base = int2(floor(g));
        float mismatch = 1e9;
        [unroll] for (int j = 0; j < 4; ++j)
            mismatch = min(mismatch, PwTDepthMismatch(depthAt, PwTHistDepth[level].Load(int3(clamp(base + int2(j & 1, j >> 1), int2(0, 0), int2(w, h) - 1), 0))));
        float weight = 1.0 - smoothstep(PwTParams.w, 3.0 * PwTParams.w, mismatch);
        float2 uv = at * PwTNative.zw;
        float3 cThen = PwTEncodeFrame(PwTHistColour[level].SampleLevel(PwTLinearClamp, uv, 0.0).rgb);
        weight *= PwTTune.x > 0.0 ? PwTColourGate(colourNow, cThen, PwTTune.x) : 1.0;
        // ...and not merely dark like it (the gate forgives that); against a 6 px average, so loosely.
        float3 a = PwTDecodeFrame(colourNow), b = PwTDecodeFrame(cThen);
        weight *= 1.0 - smoothstep(0.25, 0.5, dot(abs(a - b), 1.0) / (dot(a + b, 1.0) + 0.02));
        if (weight >= 0.25)
        {
            older = PwTHistResidual[level].SampleLevel(PwTLinearClamp, uv, 0.0).rgb;
            return any(!isfinite(older)) ? 0.0 : trust[level] * weight;
        }
    }
    return 0.0;
}
#endif

// The guide texels around a native pixel disagree in depth: the pixel is on a silhouette, and the
// guides (render resolution, jittered) cannot say which side of it.
bool PwTOnSilhouette(float2 p)
{
    float2 f = PwTToRect(p, PwTDepthRect) - 0.5;
    int2 base = int2(floor(f));
    int2 first = int2(PwTDepthRect.xy);
    int2 last = int2(PwTDepthRect.xy + PwTDepthRect.zw) - 1;
    float lo = 1e30, hi = -1e30;
    // A texel further out on every side: the upscaler's edge and the guides' differ by up to a texel.
    [unroll] for (int k = 0; k < 16; ++k)
    {
        float d = PwTDepth.Load(int3(clamp(base + int2((k & 3) - 1, (k >> 2) - 1), first, last), 0));
        lo = min(lo, d); hi = max(hi, d);
    }
    return PwTDepthMismatch(lo, hi) > PwTParams.w;
}

float4 PwTReproject(PwFullscreenVertex input, out float accepted, out float3 health)
{
    accepted = 1.0;
    health = 0.0;
    float2 p = input.position.xy;
    int2 pi = int2(p);
    float4 color = PwTLoadFrame(PwTColor, int3(PwTRectTexel(p, PwTColorRect), 0));
    // Displacement back to the residual's frame, from motion-texture pixels to native pixels.
    float dc = PwTDepth.Load(int3(PwTRectTexel(p, PwTDepthRect), 0));
    float2 acc;
    float depthThen;
    const bool silhouette = PwTTune.x > 0.0 && PwTFill.w >= 0.5;
    acc = PwTFetchAcc(p, dc);
    depthThen = PwTExpectedDepth(dc, p);
    float2 d = acc * PwTNative.xy / PwTMotionRect.zw;
    if (any(!isfinite(d))) d = float2(0.0, 0.0);
    float2 q = p + d;

    // Acceptance of the reprojected residual at p (and at four neighbours 3 px away, averaged): the
    // depth and colour tests decide per pixel, and a per-pixel decision drew a hard line across teeth
    // where the lips' old colour met the teeth's new one, flipping from frame to frame as the mouth
    // moved. Averaged over the neighbourhood the boundary becomes a 6 px ramp that does not jitter.
    float wEdge = saturate(min(min(q.x, PwTNative.x - q.x), min(q.y, PwTNative.y - q.y)) / 16.0);
    if (any(q < 0.0) || any(q >= PwTNative.xy)) wEdge = 0.0;
    float w = 0.0;
    float wCentre = 0.0;
    const bool onSilhouette = silhouette && PwTOnSilhouette(p);
    // PwTSmooth.x scales the tap radius (1 = 3 px). A wider footprint turns the accepted/filled boundary inside
    // a newly uncovered object (teeth: part seen by the full frame, part not) into a soft ramp instead of a line.
    const float tapScale = PwTSmooth.x > 0.0 ? PwTSmooth.x : 1.0;
    const float2 taps[9] = { float2(0, 0), float2(3, 0), float2(-3, 0), float2(0, 3), float2(0, -3), float2(3, 3), float2(-3, 3), float2(3, -3), float2(-3, -3) };
    const float tapWeight[9] = { 0.28, 0.12, 0.12, 0.12, 0.12, 0.06, 0.06, 0.06, 0.06 };
    [unroll] for (int k = 0; k < 9; ++k)
    {
        float2 pk = clamp(p + taps[k] * tapScale, 0.0, PwTNative.xy - 1.0);
        float2 qk = clamp(q + taps[k] * tapScale, 0.0, PwTNative.xy - 1.0);
        float dck = PwTDepth.Load(int3(PwTRectTexel(pk, PwTDepthRect), 0));
        float3 ck = PwTLoadFrame(PwTColor, int3(PwTRectTexel(pk, PwTColorRect), 0)).rgb;
        // The tap's own surface, by the frame on a silhouette (the centre's is known already).
        float thenK = k > 0 ? PwTExpectedDepth(dck, pk) : depthThen;
        float wk = 1.0 - smoothstep(PwTParams.w, 2.0 * PwTParams.w, PwTDepthMismatchAround(thenK, qk));
        if (PwTTune.x > 0.0)
        {
            // Colour consistency: the residual's frame must have shown the same thing at q (wrong vectors,
            // disocclusions the depth test misses - teeth behind lips -, HUD, transparency).
            float3 cf = PwTLoadFrame(PwTColorF, int3(PwTRectTexel(qk, PwTColorRect), 0)).rgb;
            wk *= PwTColourGate(ck, cf, PwTTune.x);
            // On a silhouette the pixel's vector may be the other surface's (the guides' edge is not the
            // frame's), and the gate above forgives too much to notice on dark things: dark wall took dark
            // hair's darkening. There the pixel itself has to look the same at both ends, strictly.
            if (k == 0 && onSilhouette) wk *= PwTSameLook(ck, cf);
        }
        w += wk * tapWeight[k];
        if (k == 0) wCentre = wk;
    }
    // The neighbourhood softens a decision, it does not overturn one: a wall pixel beside hair failed
    // its own test, but with the taps on the hair passing it still took half of what lay at the end of
    // its vector - the hair's darkening, on the wall, in steps of a guide texel along the whole outline
    // (bench12). A pixel firmly rejected on its own takes nothing from there.
    w *= saturate(2.0 * wCentre);
    w *= wEdge;
    // On a silhouette the decision is the pixel's own, whole: a half-accepted pixel is filled in part from
    // the box-filtered residual, which straddles the outline and carries the other side's contribution -
    // the remaining dark steps beside hair came from there. Inside a surface the soft ramp stays.
    if (onSilhouette) w = wCentre >= 0.5 ? wEdge : 0.0;
    // See the fill below: a rejected pixel on a silhouette cannot trust the guides about its own surface.
    const bool lone = onSilhouette && w < 0.5;

    // PwTParams.y (reprojection): 1 = Catmull-Rom resampling of the residual, 0 = bilinear.
    float2 qc0 = clamp(q, 0.0, PwTNative.xy - 1.0);
    float4 residual = PwTParams.y > 0.5 ? PwTSampleCatmullRom(PwTResidual, qc0, PwTNative.xy, PwTNative.zw)
                                       : PwTResidual.SampleLevel(PwTLinearClamp, qc0 * PwTNative.zw, 0.0);
#ifdef PW_T_RAMP
    // A new pass is phased in over a few frames (PSResidualOld): both residuals sit on the same frame, so
    // one displacement serves both.
    if (PwTSmooth.w > 0.0 && PwTSmooth.w < 1.0)
    {
        float3 older = PwTResidualOld.SampleLevel(PwTLinearClamp, qc0 * PwTNative.zw, 0.0).rgb;
        if (all(isfinite(older))) residual.rgb = lerp(older, residual.rgb, PwTSmooth.w);
    }
#endif
    // Where the reprojection is rejected (disocclusion, frame edge, wrong vector) fall back to the
    // local tone of the model: the box-filtered residual, taken where the vectors point (q), so it moves
    // with the scene. Taken at p it was the blurred residual of whatever stood at this screen position
    // in the residual's frame - a blurred ghost displaced by the whole camera motion since then.
    float3 fill = 0.0;
    float ringMatched = 0.0;
    float ringWeight = 0.0;
    if (PwTFill.x > 0.5)
    {
        float2 qc = clamp(q, 0.0, PwTNative.xy - 1.0);
        // The model's correction scales with the brightness of what it corrected: teeth uncovered by a lip
        // get the lip's blurred residual (a smaller darkening of a darker surface) and came out 10 % too
        // pale (26.6.Q). Rescale each fill sample by this pixel's luma over the luma of the residual's
        // frame where the sample comes from - an additive fill turned multiplicative where the surfaces differ.
        float lumaNow = max(dot(color.rgb, float3(0.2126, 0.7152, 0.0722)), 1e-3);
        float lumaF0 = max(dot(PwTLoadFrame(PwTColorF, int3(PwTRectTexel(qc, PwTColorRect), 0)).rgb, float3(0.2126, 0.7152, 0.0722)), 1e-3);
        // In the ratio domain the residual is a gain already and scales with nothing: no rescaling (on
        // encoded values the ratio of two lumas is a ratio of logarithms, and the correction it used to make
        // turned into a power curve - a disoccluded dark floor came out several times brighter, a pale
        // silhouette beside every character).
        const bool ratioDomain = PwTSmooth.z > 0.0;
        fill = PwTResidualLow.SampleLevel(PwTLinearClamp, qc * PwTNative.zw, 0.0).rgb * (ratioDomain ? 1.0 : clamp(lumaNow / lumaF0, 0.25, 4.0));
        if (w < 0.5)
        {
            // Disocclusion: at q the residual's frame showed the occluder (the character in front of
            // this background), so its blurred residual would trail behind it. Look around q for the
            // nearest points whose depth in the residual's frame matches this pixel's depth now - the
            // same surface - and take the blurred residual there; found nothing: no residual (plain
            // colour) rather than a wrong one.
            // Both rings, weighted by how well the depth matches: a single nearest hit would change
            // from frame to frame and flicker.
            float3 sum = 0.0;
            float n = 0.0;
            // What this surface's depth was back then, where that is known: the same surface is looked for.
            float dcThen = !isfinite(depthThen) ? dc : depthThen;
            // Four rings, 24 to 192 px: a third-person character a few metres from the camera is hundreds of
            // pixels wide, and two rings of 24/48 px around a point behind him never left his silhouette.
            float matched = 0.0;
            // The rings turn by a per-pixel angle (interleaved gradient noise): eight fixed directions drew
            // the occluder's outline as a sawtooth of triangles on the floor behind him; turned, the same
            // error is fine noise that the compose pass averages away.
            const float turn = 0.785398 * frac(52.9829189 * frac(0.06711056 * p.x + 0.00583715 * p.y));
            [unroll] for (int ring = 1; ring <= 4; ++ring)
            {
                float r = 12.0 * (float) (1 << ring);
                [unroll] for (int k = 0; k < 8; ++k)
                {
                    float ang = k * 0.785398 + ((ring & 1) == 0 ? 0.392699 : 0.0) + turn;
                    float2 qk = clamp(qc + float2(cos(ang), sin(ang)) * r, 0.0, PwTNative.xy - 1.0);
                    float dfk = PwTDepthF.Load(int3(PwTRectTexel(qk, PwTDepthRect), 0));
                    float wk = 1.0 - smoothstep(PwTParams.w, 2.0 * PwTParams.w, PwTDepthMismatch(dcThen, dfk));
                    // ...and by colour: the residual's frame should have shown something like this pixel
                    // there (teeth uncovered by lips: the lip's blurred residual is not the teeth's tone).
                    float3 cfk = PwTLoadFrame(PwTColorF, int3(PwTRectTexel(qk, PwTColorRect), 0)).rgb;
                    float gate = PwTTune.x > 0.0 ? PwTColourGate(color.rgb, cfk, 2.0 * PwTTune.x) : 1.0;
                    // On a silhouette the pixel's depth is a guess (lone): the frame alone says which
                    // neighbours are the same surface, and it has to say so clearly.
                    wk *= gate;
                    matched += wk;
                    if (PwTTune.w > 0.5) wk = max(wk, 0.05);
                    // Default: no floor. With 32 taps a floor of 0.05 adds up to 1.6, and where two
                    // taps reached the floor behind a character the other thirty put 40 % of his jacket's
                    // contribution - pockets and buttons included - onto that floor (007 First Light, a
                    // translucent copy of the jacket beside him). Whether enough matched is decided below.
                    float ratio = ratioDomain ? 1.0 : clamp(lumaNow / max(dot(cfk, float3(0.2126, 0.7152, 0.0722)), 1e-3), 0.25, 4.0);
                    // Where the colour matches, take the residual of that very spot (bilinear) rather than the 16 px box
                    // average, which mixes the matching surface with its neighbours (teeth with lips: too pale).
                    float3 rk = lerp(PwTResidualLow.SampleLevel(PwTLinearClamp, qk * PwTNative.zw, 0.0).rgb,
                                     PwTResidual.SampleLevel(PwTLinearClamp, qk * PwTNative.zw, 0.0).rgb, gate);
                    sum += rk * ratio * wk;
                    n += wk;
                }
            }
            fill = n > 1e-3 ? sum / n : 0.0;
            // Nothing around shows this pixel's surface: the floored average is then the occluder's own
            // contribution, and on the floor behind a dark jacket it drew the jacket's gain as a pale
            // silhouette. No match, no borrowed contribution - the pixel shows the game's frame until the
            // model looks again.
            if (PwTTune.w <= 0.5) fill *= saturate(matched / 0.15);
            ringMatched = matched;
            ringWeight = n;
        }
        // The fill carries the chroma of whatever the residual's frame showed around q (a lip's residual on
        // teeth turned them greenish); keep only its luma and apply it along this pixel's own chroma.
        // (ratio domain: the same gain on every channel keeps the pixel's chroma by construction)
        float lf = dot(fill, float3(0.2126, 0.7152, 0.0722));
        fill = ratioDomain ? float3(lf, lf, lf) : color.rgb * (lf / lumaNow);
    }
    // NaN in the residual or the fill would survive a zero weight (0 * NaN): never let it reach the frame.
    if (any(!isfinite(residual.rgb))) residual.rgb = 0.0;
    if (any(!isfinite(fill))) fill = 0.0;
    // A rejected pixel borrows its neighbours' contribution (the fill here, the compose pass after) from
    // "the same surface", and which surface that is comes from the guides. On a silhouette they are wrong
    // for every other pixel: wall beside hair borrowed the hair's darkening, in steps of a guide texel
    // along the whole outline, from either pass alone (bench12). There a rejected pixel borrows
    // only from neighbours the frame itself shows alike, here and in the compose pass (told by a negative
    // acceptance).
    if (lone) fill = 0.0; // painted by the compose pass, from the frame's own look
    // Raw acceptance and ring availability BEFORE history/cells/compose recovery.
    // noFill means no usable ring contribution, not a numerically zero residual.
    health = float3(saturate(w), w < 0.5 && ringMatched <= 1e-3 ? 1.0 : 0.0,
        w < 0.5 && (lone || PwTFill.x <= 0.5 || ringWeight <= 1e-3 ||
                    (PwTTune.w <= 0.5 && ringMatched <= 1e-3)) ? 1.0 : 0.0);
    float fromOlder = 0.0;
#ifdef PW_T_HISTORY
    // PwTParams.x (reprojection): how many older passes are on hand.
    if (PwTParams.x > 0.5 && w < 0.5)
    {
        float3 older;
        float depthThere = dc;
#ifdef PW_T_EXPECT
        float geometric = PwTExpect.Load(int3(PwTRectTexel(p, PwTMotionRect), 0)).z;
        if (isfinite(geometric)) depthThere = geometric;
#endif
        fromOlder = PwTFromOlderPasses(clamp(q, 0.0, PwTNative.xy - 1.0), depthThere, color.rgb, (int) PwTParams.x, older);
        fill = lerp(fill, older, fromOlder);
    }
#endif
    float3 add = lerp(fill, residual.rgb, w);
    if (any(!isfinite(add))) add = 0.0;
    if (PwTTune.z > 0.5) add = 0.0; // diagnostics: interpolated frames show the raw colour
    // -1: rejected on a silhouette, the compose pass goes by colour alone there. What the older pass
    // supplied is the model's own and counts as settled: the compose pass leaves it, the cells learn from it.
    accepted = (lone && fromOlder < 0.5) ? -1.0 : max(saturate(w), fromOlder);
    float4 reprojected = float4(color.rgb + add, color.a);
    // Diagnostics (PW_NGX_TEMPORAL_VIS): 1 displacement and weight, 2 residual, 3 raw colour.
    if (PwTParams.z == 1.0) return float4(0.0, abs(d.y) / 64.0, w, 1.0); // B: acceptance
    if (PwTParams.z == 2.0) return float4(residual.rgb * 4.0 + 0.5, 1.0);
    if (PwTParams.z == 3.0) return color;
    if (PwTParams.z == 6.0) return float4(fromOlder * 50.0, (w < 0.5 ? 1.0 : 0.0) * 50.0, lone ? 50.0 : 0.0, 1.0); // R: supplied by the older pass, G: rejected, B: on a silhouette
    if (PwTParams.z == 5.0) return float4(abs(d.x) / 4.0, abs(d.y) / 4.0, 0.0, 1.0); // displacement, full scale = 4 native px
    if (PwTParams.z == 4.0)
    {
        // Depth diagnostics: R = this frame's depth stretched over its local range, B = relative mismatch
        // with the residual frame's depth at q, x20.
        float dfq = PwTDepthF.Load(int3(PwTRectTexel(clamp(q, 0.0, PwTNative.xy - 1.0), PwTDepthRect), 0));
        float rel = PwTDepthMismatch(dc, dfq);
        return float4(dc, 0.0, saturate(rel * 20.0), 1.0);
    }

    return reprojected;
}

// Second target (26.6.X): what the reprojection ADDED to the colour (rgb) and how much of it was accepted (a).
// The compose pass smooths the addition along the original's own smoothness.
struct PwTReprojectOut { float4 color : SV_Target0; float4 addw : SV_Target1; };
PwTReprojectOut PSReproject(PwFullscreenVertex input)
{
    PwTReprojectOut o;
    float accepted;
    float3 health;
    o.color = PwTReproject(input, accepted, health);
    float3 base = PwTLoadFrame(PwTColor, int3(PwTRectTexel(input.position.xy, PwTColorRect), 0)).rgb;
    o.addw = float4(o.color.rgb - base, accepted);
    if (PwTParams.z != 0.0) o.addw = float4(0, 0, 0, 1); // diagnostics: nothing to smooth
    return o;
}

#ifdef PW_T_CELLS
// Cells of what the reprojection ACCEPTED on this frame, for the pixels it rejected (PSCompose).
//
// A rejected pixel used to find its contribution by itself: taps around it, each tested against it, a
// different set for every pixel. Where few taps match - the gap between a walker's legs, the band a moving
// outline uncovers - one pixel found a match and its neighbour did not, and the band came out as a
// checkerboard of corrected and uncorrected pixels that changed every frame (bench12). Here the accepted
// addition is averaged once per cell of the low-resolution grid, with the mean look of the pixels it came
// from; a rejected pixel then takes a smooth blend of the cells around it, weighted by how much its own
// colour resembles each cell's. Nothing is decided per pixel, so nothing dithers.
// Runs over the low-resolution grid. t0 = the frame, t2 = the reprojection's addition and acceptance.
struct PwTCellsOut { float4 add : SV_Target0; float4 look : SV_Target1; };
PwTCellsOut PSCells(PwFullscreenVertex input)
{
    float2 block = PwTNative.xy / PwTFill.yz;
    float2 origin = (input.position.xy - 0.5) * block;
    float3 sumAdd = 0.0, sumLook = 0.0;
    float sumW = 0.0;
    [loop] for (int y = 0; y < 8; ++y)
        [loop] for (int x = 0; x < 8; ++x)
        {
            float2 p = min(origin + (float2(x, y) + 0.5) * block * 0.125, PwTNative.xy - 1.0);
            float4 aw = PwTResidual.Load(int3(int2(p), 0));
            float a = saturate(aw.a);
            float w = a * a; // firmly accepted pixels speak for the cell
            if (any(!isfinite(aw.rgb))) w = 0.0;
            sumAdd += aw.rgb * w;
            sumLook += PwTLoadFrame(PwTColor, int3(PwTRectTexel(p, PwTColorRect), 0)).rgb * w;
            sumW += w;
        }
    PwTCellsOut o;
    o.add = float4(sumW > 1e-4 ? sumAdd / sumW : float3(0.0, 0.0, 0.0), sumW / 64.0);
    o.look = float4(sumW > 1e-4 ? sumLook / sumW : float3(0.0, 0.0, 0.0), 0.0);
    return o;
}

// The blend of the cells around a native pixel for a pixel of this look (encoded colour). .a = how much
// there was to blend (0 = nothing alike nearby). t8 = the cells' addition and share, t1 = their look.
float4 PwTFromCells(float2 p, float3 encodedColour)
{
    float2 block = PwTNative.xy / PwTFill.yz;
    float2 g = p / block - 0.5;
    int2 centre = int2(round(g));
    int2 last = int2(PwTFill.yz) - 1;
    float3 sum = 0.0;
    float wsum = 0.0;
    float3 cNow = PwTDecodeFrame(encodedColour);
    [unroll] for (int y = -1; y <= 1; ++y)
        [unroll] for (int x = -1; x <= 1; ++x)
        {
            int2 cell = centre + int2(x, y);
            if (any(cell < 0) || any(cell > last)) continue;
            float2 away = abs(g - float2(cell));
            float tent = saturate(1.0 - away.x / 1.5) * saturate(1.0 - away.y / 1.5);
            float4 add = PwTResidualLow.Load(int3(cell, 0));
            float3 cCell = PwTDecodeFrame(PwTFresh.Load(int3(cell, 0)).rgb);
            // A cell is a 16 px average: texture and noise are forgiven, another surface is not.
            float alike = 1.0 - smoothstep(0.12, 0.30, dot(abs(cNow - cCell), 1.0) / (dot(cNow + cCell, 1.0) + 0.02));
            float w = tent * add.a * alike;
            sum += add.rgb * w;
            wsum += w;
        }
    return float4(wsum > 1e-4 ? sum / wsum : float3(0.0, 0.0, 0.0), saturate(wsum / 0.08));
}
#endif

// Compose (26.6.X): the interpolated frame is colour + the reprojection's addition, but the addition is
// smoothed along the ORIGINAL frame's smoothness: taps within the radius that match this pixel in colour
// and depth (the same surface in the game's own frame) are averaged, so a step in the addition where the
// original has none (the line across the teeth between the part the full pass saw and the part it did not,
// the rim of a reopened eye) is flattened. Fully accepted neighbourhoods keep the addition as is - the
// smoothing strength is 1 - the minimum acceptance around the pixel, so ordinary surfaces lose no detail.
// The addition is bound in the residual's slot (t2 = PwTResidual) for this pass.
float4 PSCompose(PwFullscreenVertex input) : SV_Target0
{
    float2 p = input.position.xy;
    int2 pi = int2(p);
    float4 color = PwTLoadFrame(PwTColor, int3(PwTRectTexel(p, PwTColorRect), 0));
    float4 aw = PwTResidual.Load(int3(pi, 0));
    float radius = PwTSmooth.y;
    if (radius <= 0.0) return float4(color.rgb + aw.rgb, color.a);
    float dc = PwTDepth.Load(int3(PwTRectTexel(p, PwTDepthRect), 0));
    const float2 taps[16] = { float2(1, 0), float2(-1, 0), float2(0, 1), float2(0, -1), float2(0.7, 0.7), float2(-0.7, 0.7), float2(0.7, -0.7), float2(-0.7, -0.7),
                              float2(0.5, 0), float2(-0.5, 0), float2(0, 0.5), float2(0, -0.5), float2(0.35, 0.35), float2(-0.35, 0.35), float2(0.35, -0.35), float2(-0.35, -0.35) };
    float3 sum = aw.rgb; float wsum = 1.0;
    const bool lone = aw.a < -0.5;
    float tol = PwTTune.x > 0.0 ? PwTTune.x : 0.08;
    // Rotate the tap pattern per pixel (interleaved gradient noise): fixed taps at two radii printed the
    // lip line as thin streaks across the teeth; rotated, the same error becomes fine noise.
    float ang = 6.2831853 * frac(52.9829189 * frac(0.06711056 * p.x + 0.00583715 * p.y));
    float2 rot = float2(cos(ang), sin(ang));
    [unroll] for (int k = 0; k < 16; ++k)
    {
        float2 tk = float2(taps[k].x * rot.x - taps[k].y * rot.y, taps[k].x * rot.y + taps[k].y * rot.x);
        float2 pk = clamp(p + tk * radius, 0.0, PwTNative.xy - 1.0);
        float4 awk = PwTResidual.Load(int3(int2(pk), 0));
        float3 ck = PwTLoadFrame(PwTColor, int3(PwTRectTexel(pk, PwTColorRect), 0)).rgb;
        float dk = PwTDepth.Load(int3(PwTRectTexel(pk, PwTDepthRect), 0));
        // A pixel rejected on a silhouette (see PwTReproject) cannot go by its depth: colour alone, strictly.
        float wk = lone ? PwTSameLook(color.rgb, ck)
                        : PwTColourGate(color.rgb, ck, tol) * (1.0 - smoothstep(PwTParams.w, 2.0 * PwTParams.w, PwTDepthMismatch(dc, dk)));
        wk *= (k < 8) ? 0.6 : 1.0;       // the outer ring weighs a little less
        wk *= 0.2 + 0.8 * saturate(awk.a);         // neighbours that carry the model's own result weigh more than other fills
        sum += awk.rgb * wk; wsum += wk;
    }
    float3 blurred = sum / wsum;
#ifdef PW_T_CELLS
    // Where the cells around have something that looks like this pixel, they paint it (see PSCells); the
    // taps above remain for where they have not.
    if (PwTFill.x > 0.5)
    {
        float4 cells = PwTFromCells(p, color.rgb);
        if (all(isfinite(cells))) blurred = lerp(blurred, cells.rgb, cells.a);
    }
#endif
    // Only pixels WITHOUT the model's result are painted from their surroundings; accepted pixels keep the
    // reprojected model output untouched (the user's rule: what the model rendered is transferred as is).
    // Partial acceptance (0.5..0.9, common in the background mode where the residual is 3-8 frames old) still
    // carries the model's result: only pixels that were essentially rejected are painted. Blending the smoothed
    // addition into partially accepted pixels shifted whole surfaces (forward run, background mode: 1.7 -> 2.8).
    float s = 1.0 - smoothstep(0.2, 0.5, saturate(aw.a));
    float3 add = lerp(aw.rgb, blurred, s);
    if (any(!isfinite(add))) add = aw.rgb;
    return float4(color.rgb + add, color.a);
}
