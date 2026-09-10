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
    float4 PwTTune;        // colour tolerance (relative luma, 0 = off), motion sign (+1/-1), raw interpolation (0/1), residual blend weight (26.6.K)
    float4 PwTFill;        // hole fill with the low-res residual (0/1), low-res width, low-res height, depth-guided chain fetch (0 off, 1 four texels, >1 = search radius in native px)
    float4 PwTSmooth;      // acceptance tap radius scale (0 = 1), guided smoothing radius in px (26.6.X, 0 = off), unused, unused
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
SamplerState PwTLinearClamp : register(s0);
SamplerState PwTPointClamp : register(s1);

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
    return result / max(norm, 1e-4);
}

float2 PwTToRect(float2 nativePixel, float4 rect)
{
    return rect.xy + nativePixel * PwTNative.zw * rect.zw;
}

// Residual of a full pass.
// Colour consistency between this frame's colour c and the residual frame's colour cf at the reprojected
// point, as a weight 1 (same thing) .. 0 (something else). Chromaticity first: a change of lighting
// (a turning face, a moving light) changes the luma of a surface but hardly its hue, while a
// disocclusion (teeth behind lips, red against white; skin against hair) changes the hue. Luma is
// tested too, with twice the tolerance, for same-hue disocclusions and moving shadows.
float PwTColourGate(float3 c, float3 cf, float tol)
{
    float3 nc = c / (dot(c, 1.0) + 1e-3), nf = cf / (dot(cf, 1.0) + 1e-3);
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
    int2 c = int2(PwTToRect(input.position.xy, PwTColorRect));
    float4 fresh = PwTFresh.Load(int3(p, 0));
    float4 color = PwTColor.Load(int3(c, 0));
    float3 r = fresh.rgb - color.rgb;
    if (PwTTune.w > 0.0)
    {
        float2 mvPixel = PwTToRect(input.position.xy, PwTMotionRect);
        float2 acc = PwTAccPrev.SampleLevel(PwTLinearClamp, mvPixel / PwTMotionTex.xy, 0.0);
        float2 q = input.position.xy + acc * PwTNative.xy / PwTMotionRect.zw;
        if (all(isfinite(q)) && all(q >= 0.0) && all(q < PwTNative.xy))
        {
            float dc = PwTDepth.Load(int3(int2(PwTToRect(input.position.xy, PwTDepthRect)), 0));
            float df = PwTDepthF.Load(int3(int2(PwTToRect(q, PwTDepthRect)), 0));
            float den = max(max(abs(dc), abs(df)), 1e-6);
            float wd = 1.0 - smoothstep(PwTParams.w * den, 2.0 * PwTParams.w * den, abs(dc - df));
            // Adaptive: full weight where the pixel barely moved between the passes (a face in a cutscene:
            // the model's pass-to-pass jitter of fine detail is what flickers there) and none where it
            // moved more than a few pixels or the colour under it changed (lips, a turning head, walking),
            // so nothing lags or trails.
            float2 d = q - input.position.xy;
            float motionFade = 1.0 - smoothstep(2.0, 6.0, length(d));
            float colourGate = 1.0;
            if (PwTTune.x > 0.0)
            {
                float3 cf = PwTColorF.Load(int3(int2(PwTToRect(q, PwTColorRect)), 0)).rgb;
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
                    float3 rn = PwTFresh.Load(int3(pn, 0)).rgb - PwTColor.Load(int3(int2(PwTToRect(float2(pn) + 0.5, PwTColorRect)), 0)).rgb;
                    mn = min(mn, rn); mx = max(mx, rn);
                }
            old = clamp(old, mn, mx);
            if (all(isfinite(old))) r = lerp(r, old, PwTTune.w * wd * motionFade * colourGate);
        }
    }
    if (any(!isfinite(r))) r = fresh.rgb - color.rgb;
    return float4(r, 0.0);
}

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
        float2 uv = prev / PwTMotionTex.xy;
        acc += PwTAccPrev.SampleLevel(PwTLinearClamp, uv, 0.0);
        if (any(!isfinite(acc))) acc = mv;
        if (PwTParams.x > 0.5)
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
                float dc = PwTDepth.Load(int3(int2(PwTToRect(nativePixel, PwTDepthRect)), 0));
                float df = PwTDepthF.Load(int3(int2(PwTToRect(qN, PwTDepthRect)), 0));
                float den = max(max(abs(dc), abs(df)), 1e-6);
                bool ok = abs(dc - df) <= 2.0 * PwTParams.w * den;
                if (ok && PwTTune.x > 0.0)
                {
                    float3 c = PwTColor.Load(int3(int2(PwTToRect(nativePixel, PwTColorRect)), 0)).rgb;
                    float3 cf = PwTColorF.Load(int3(int2(PwTToRect(qN, PwTColorRect)), 0)).rgb;
                    ok = PwTColourGate(c, cf, PwTTune.x) > 0.05;
                }
                if (!ok) acc = mv;
            }
        }
    }
    return acc;
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
    float2 result = PwTAccPrev.SampleLevel(PwTLinearClamp, mvPixel / PwTMotionTex.xy, 0.0);
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
        int2 tex = clamp(int2(base) + o, int2(0, 0), int2(PwTMotionTex.xy) - 1);
        float2 nativePixel = (float2(tex) + 0.5 - PwTMotionRect.xy) / PwTMotionRect.zw * PwTNative.xy;
        float dt = PwTDepth.Load(int3(int2(PwTToRect(nativePixel, PwTDepthRect)), 0));
        float diff = abs(dt - dc) / max(max(abs(dt), abs(dc)), 1e-6);
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
                int2 tex = clamp(int2(floor(mvPixel + offs * toMv)), int2(0, 0), int2(PwTMotionTex.xy) - 1);
                float2 nativePixel = (float2(tex) + 0.5 - PwTMotionRect.xy) / PwTMotionRect.zw * PwTNative.xy;
                float dt = PwTDepth.Load(int3(int2(PwTToRect(nativePixel, PwTDepthRect)), 0));
                float diff = abs(dt - dc) / max(max(abs(dt), abs(dc)), 1e-6);
                if (diff < bestDiff) { bestDiff = diff; closest = PwTAccPrev.Load(int3(tex, 0)); }
            }
            if (bestDiff <= PwTParams.w) break;
        }
    }
    result = (wsum > 1e-4) ? (acc / wsum) : closest;
    if (any(!isfinite(result))) result = float2(0.0, 0.0);
    return result;
}

float4 PwTReproject(PwFullscreenVertex input, out float accepted)
{
    accepted = 1.0;
    float2 p = input.position.xy;
    int2 pi = int2(p);
    float4 color = PwTColor.Load(int3(int2(PwTToRect(p, PwTColorRect)), 0));
    // Displacement back to the residual's frame, from motion-texture pixels to native pixels.
    float dc = PwTDepth.Load(int3(int2(PwTToRect(p, PwTDepthRect)), 0));
    float2 acc = PwTFetchAcc(p, dc);
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
    // PwTSmooth.x scales the tap radius (1 = 3 px). A wider footprint turns the accepted/filled boundary inside
    // a newly uncovered object (teeth: part seen by the full frame, part not) into a soft ramp instead of a line.
    const float tapScale = PwTSmooth.x > 0.0 ? PwTSmooth.x : 1.0;
    const float2 taps[9] = { float2(0, 0), float2(3, 0), float2(-3, 0), float2(0, 3), float2(0, -3), float2(3, 3), float2(-3, 3), float2(3, -3), float2(-3, -3) };
    const float tapWeight[9] = { 0.28, 0.12, 0.12, 0.12, 0.12, 0.06, 0.06, 0.06, 0.06 };
    [unroll] for (int k = 0; k < 9; ++k)
    {
        float2 pk = clamp(p + taps[k] * tapScale, 0.0, PwTNative.xy - 1.0);
        float2 qk = clamp(q + taps[k] * tapScale, 0.0, PwTNative.xy - 1.0);
        float dck = PwTDepth.Load(int3(int2(PwTToRect(pk, PwTDepthRect)), 0));
        float dfk = PwTDepthF.Load(int3(int2(PwTToRect(qk, PwTDepthRect)), 0));
        float denom = max(max(abs(dck), abs(dfk)), 1e-6);
        float wk = 1.0 - smoothstep(PwTParams.w * denom, 2.0 * PwTParams.w * denom, abs(dck - dfk));
        if (PwTTune.x > 0.0)
        {
            // Colour consistency: the residual's frame must have shown the same thing at q (wrong vectors,
            // disocclusions the depth test misses - teeth behind lips -, HUD, transparency).
            float3 ck = PwTColor.Load(int3(int2(PwTToRect(pk, PwTColorRect)), 0)).rgb;
            float3 cf = PwTColorF.Load(int3(int2(PwTToRect(qk, PwTColorRect)), 0)).rgb;
            wk *= PwTColourGate(ck, cf, PwTTune.x);
        }
        w += wk * tapWeight[k];
    }
    w *= wEdge;

    // PwTParams.y (reprojection): 1 = Catmull-Rom resampling of the residual, 0 = bilinear.
    float2 qc0 = clamp(q, 0.0, PwTNative.xy - 1.0);
    float4 residual = PwTParams.y > 0.5 ? PwTSampleCatmullRom(PwTResidual, qc0, PwTNative.xy, PwTNative.zw)
                                       : PwTResidual.SampleLevel(PwTLinearClamp, qc0 * PwTNative.zw, 0.0);
    // Where the reprojection is rejected (disocclusion, frame edge, wrong vector) fall back to the
    // local tone of the model: the box-filtered residual, taken where the vectors point (q), so it moves
    // with the scene. Taken at p it was the blurred residual of whatever stood at this screen position
    // in the residual's frame - a blurred ghost displaced by the whole camera motion since then.
    float3 fill = 0.0;
    if (PwTFill.x > 0.5)
    {
        float2 qc = clamp(q, 0.0, PwTNative.xy - 1.0);
        // The model's correction scales with the brightness of what it corrected: teeth uncovered by a lip
        // get the lip's blurred residual (a smaller darkening of a darker surface) and came out 10 % too
        // pale (26.6.Q). Rescale each fill sample by this pixel's luma over the luma of the residual's
        // frame where the sample comes from - an additive fill turned multiplicative where the surfaces differ.
        float lumaNow = max(dot(color.rgb, float3(0.2126, 0.7152, 0.0722)), 1e-3);
        float lumaF0 = max(dot(PwTColorF.Load(int3(int2(PwTToRect(qc, PwTColorRect)), 0)).rgb, float3(0.2126, 0.7152, 0.0722)), 1e-3);
        fill = PwTResidualLow.SampleLevel(PwTLinearClamp, qc * PwTNative.zw, 0.0).rgb * clamp(lumaNow / lumaF0, 0.25, 4.0);
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
            [unroll] for (int ring = 1; ring <= 2; ++ring)
            {
                float r = 24.0 * ring;
                [unroll] for (int k = 0; k < 8; ++k)
                {
                    float ang = k * 0.785398 + (ring == 2 ? 0.392699 : 0.0);
                    float2 qk = clamp(qc + float2(cos(ang), sin(ang)) * r, 0.0, PwTNative.xy - 1.0);
                    float dfk = PwTDepthF.Load(int3(int2(PwTToRect(qk, PwTDepthRect)), 0));
                    float den = max(max(abs(dc), abs(dfk)), 1e-6);
                    // A small floor keeps the fill defined (and steady) where no neighbour matches: the
                    // plain ring average rather than no residual at all, which is 5-8 % darker than its
                    // surroundings and flickered against neighbours that had a match.
                    float wk = 1.0 - smoothstep(PwTParams.w * den, 2.0 * PwTParams.w * den, abs(dc - dfk));
                    // ...and by colour: the residual's frame should have shown something like this pixel
                    // there (teeth uncovered by lips: the lip's blurred residual is not the teeth's tone).
                    float3 cfk = PwTColorF.Load(int3(int2(PwTToRect(qk, PwTColorRect)), 0)).rgb;
                    float gate = PwTTune.x > 0.0 ? PwTColourGate(color.rgb, cfk, 2.0 * PwTTune.x) : 1.0;
                    wk = max(wk * gate, 0.05);
                    float ratio = clamp(lumaNow / max(dot(cfk, float3(0.2126, 0.7152, 0.0722)), 1e-3), 0.25, 4.0);
                    // Where the colour matches, take the residual of that very spot (bilinear) rather than the 16 px box
                    // average, which mixes the matching surface with its neighbours (teeth with lips: too pale).
                    float3 rk = lerp(PwTResidualLow.SampleLevel(PwTLinearClamp, qk * PwTNative.zw, 0.0).rgb,
                                     PwTResidual.SampleLevel(PwTLinearClamp, qk * PwTNative.zw, 0.0).rgb, gate);
                    sum += rk * ratio * wk;
                    n += wk;
                }
            }
            fill = n > 1e-3 ? sum / n : 0.0;
        }
        // The fill carries the chroma of whatever the residual's frame showed around q (a lip's residual on
        // teeth turned them greenish); keep only its luma and apply it along this pixel's own chroma.
        float lf = dot(fill, float3(0.2126, 0.7152, 0.0722));
        fill = color.rgb * (lf / lumaNow);
    }
    // NaN in the residual or the fill would survive a zero weight (0 * NaN): never let it reach the frame.
    if (any(!isfinite(residual.rgb))) residual.rgb = 0.0;
    if (any(!isfinite(fill))) fill = 0.0;
    float3 add = lerp(fill, residual.rgb, w);
    if (any(!isfinite(add))) add = 0.0;
    if (PwTTune.z > 0.5) add = 0.0; // diagnostics: interpolated frames show the raw colour
    accepted = saturate(w);
    float4 reprojected = float4(color.rgb + add, color.a);
    // Diagnostics (PW_NGX_TEMPORAL_VIS): 1 displacement and weight, 2 residual, 3 raw colour.
    if (PwTParams.z == 1.0) return float4(0.0, abs(d.y) / 64.0, w, 1.0); // B: acceptance
    if (PwTParams.z == 2.0) return float4(residual.rgb * 4.0 + 0.5, 1.0);
    if (PwTParams.z == 3.0) return color;
    if (PwTParams.z == 4.0)
    {
        // Depth diagnostics: R = this frame's depth stretched over its local range, B = relative mismatch
        // with the residual frame's depth at q, x20.
        float dfq = PwTDepthF.Load(int3(int2(PwTToRect(clamp(q, 0.0, PwTNative.xy - 1.0), PwTDepthRect)), 0));
        float rel = abs(dc - dfq) / max(max(abs(dc), abs(dfq)), 1e-6);
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
    o.color = PwTReproject(input, accepted);
    float3 base = PwTColor.Load(int3(int2(PwTToRect(input.position.xy, PwTColorRect)), 0)).rgb;
    o.addw = float4(o.color.rgb - base, accepted);
    if (PwTParams.z != 0.0) o.addw = float4(0, 0, 0, 1); // diagnostics: nothing to smooth
    return o;
}

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
    float4 color = PwTColor.Load(int3(int2(PwTToRect(p, PwTColorRect)), 0));
    float4 aw = PwTResidual.Load(int3(pi, 0));
    float radius = PwTSmooth.y;
    if (radius <= 0.0) return float4(color.rgb + aw.rgb, color.a);
    float dc = PwTDepth.Load(int3(int2(PwTToRect(p, PwTDepthRect)), 0));
    const float2 taps[16] = { float2(1, 0), float2(-1, 0), float2(0, 1), float2(0, -1), float2(0.7, 0.7), float2(-0.7, 0.7), float2(0.7, -0.7), float2(-0.7, -0.7),
                              float2(0.5, 0), float2(-0.5, 0), float2(0, 0.5), float2(0, -0.5), float2(0.35, 0.35), float2(-0.35, 0.35), float2(0.35, -0.35), float2(-0.35, -0.35) };
    float3 sum = aw.rgb; float wsum = 1.0;
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
        float3 ck = PwTColor.Load(int3(int2(PwTToRect(pk, PwTColorRect)), 0)).rgb;
        float dk = PwTDepth.Load(int3(int2(PwTToRect(pk, PwTDepthRect)), 0));
        float den = max(max(abs(dc), abs(dk)), 1e-6);
        float wk = PwTColourGate(color.rgb, ck, tol) * (1.0 - smoothstep(PwTParams.w * den, 2.0 * PwTParams.w * den, abs(dc - dk)));
        wk *= (k < 8) ? 0.6 : 1.0;       // the outer ring weighs a little less
        wk *= 0.2 + 0.8 * awk.a;         // neighbours that carry the model's own result weigh more than other fills
        sum += awk.rgb * wk; wsum += wk;
    }
    float3 blurred = sum / wsum;
    // Only pixels WITHOUT the model's result are painted from their surroundings; accepted pixels keep the
    // reprojected model output untouched (the user's rule: what the model rendered is transferred as is).
    // Partial acceptance (0.5..0.9, common in the background mode where the residual is 3-8 frames old) still
    // carries the model's result: only pixels that were essentially rejected are painted. Blending the smoothed
    // addition into partially accepted pixels shifted whole surfaces (forward run, background mode: 1.7 -> 2.8).
    float s = 1.0 - smoothstep(0.2, 0.5, aw.a);
    float3 add = lerp(aw.rgb, blurred, s);
    if (any(!isfinite(add))) add = aw.rgb;
    return float4(color.rgb + add, color.a);
}
