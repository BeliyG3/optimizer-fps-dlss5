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

