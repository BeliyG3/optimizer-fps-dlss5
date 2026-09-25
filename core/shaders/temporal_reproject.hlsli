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
