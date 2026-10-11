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

// This frame's vector of the motion texel at `position` (its centre), in motion-texture pixels (already scaled).
float2 PwTFrameVector(float2 position)
{
    float2 mv = PwTMotion.Load(int3(int2(position), 0)) * PwTMotionTex.zw * PwTTune.y;
    if (any(!isfinite(mv))) mv = float2(0.0, 0.0); // a NaN vector would poison the chain until the next full pass
    return mv;
}

// The chain `chain` holds, continued at `position` by this frame's vector `mv`, before the link is tested.
float2 PwTChainContinued(Texture2D<float2> chain, float2 position, float2 mv)
{
    float2 acc = mv;
    if (PwTParams.y > 0.5)
    {
        acc += PwTSampleChainAt(chain, position + mv);
        if (any(!isfinite(acc))) acc = mv;
    }
    return acc;
}

// The chain of the motion texel at `position` (its centre) before its link is tested, and this frame's vector `mv`,
// both in motion-texture pixels (already scaled). PSAccumulate and PSLinkFail.
float2 PwTChainUntested(float2 position, out float2 mv)
{
    mv = PwTFrameVector(position);
    return PwTChainContinued(PwTAccPrev, position, mv);
}

// Whether PSAccumulate tests the link at `position` at all: the main chain (PwTParams.x), a previous chain to inherit
// (PwTParams.y) and a surface the residual's frame showed. A surface it did not show has nothing to be validated
// against; the reprojection rejects it by its unknown depth. (Lending such a pixel the chain of its neighbours, so
// that it knows where it lay under the occluder, was tried: the older passes' lookup finds its way from the
// neighbours' links anyway, and in forward flight the lent chains cost 2 % of the whole frame.)
bool PwTLinkTested(float2 position)
{
    float2 nativeHere = (position - PwTMotionRect.xy) / PwTMotionRect.zw * PwTNative.xy;
    float depthHere = PwTDepth.Load(int3(PwTRectTexel(nativeHere, PwTDepthRect), 0));
    return PwTParams.y > 0.5 && PwTParams.x > 0.5 && isfinite(PwTExpectedDepth(depthHere, nativeHere));
}

// Validate the link (main chain only): the chain is inherited through occluders - a pixel the lower lip just uncovered
// takes over the lip's accumulated displacement and lands on the lip in the residual's frame; every frame of the
// opening mouth added another copy of the lip line across the teeth (26.6.Q, cutegirl bench). If the residual's frame
// does not show this pixel's surface at the chain's end (depth or colour), the chain starts afresh from this frame's
// vector: the reprojection then rejects it honestly and fills locally. Only where the chain's end is in the frame.
// DebugTemporalChainRule `rule` (PwTSky.z on Accumulate) decides which change restarts it, before rule 3's vote:
// 0 a depth mismatch above twice the tolerance or the colour gate (today); 2 the colour gate only with a depth hint
// (a mismatch of at least the tolerance, PwTParams.w); 4 depth only. Rule 1 never restarts and does not call this.
bool PwTLinkFails(float2 position, float2 acc, int rule)
{
    float2 nativePixel = (position - PwTMotionRect.xy) / PwTMotionRect.zw * PwTNative.xy;
    float2 qN = nativePixel + acc * PwTNative.xy / PwTMotionRect.zw;
    bool inside = all(qN >= 0.0) && all(qN < PwTNative.xy);
    if (!inside) return false;
    float dc = PwTDepth.Load(int3(PwTRectTexel(nativePixel, PwTDepthRect), 0));
    float mismatch = PwTDepthMismatchAround(PwTExpectedDepth(dc, nativePixel), qN);
    bool ok = mismatch <= 2.0 * PwTParams.w; // a NaN mismatch fails, as it always did
    if (ok && PwTTune.x > 0.0 && rule != 4 && (rule == 0 || mismatch >= PwTParams.w))
    {
        float3 c = PwTLoadFrame(PwTColor, int3(PwTRectTexel(nativePixel, PwTColorRect), 0)).rgb;
        float3 cf = PwTLoadFrame(PwTColorF, int3(PwTRectTexel(qN, PwTColorRect), 0)).rgb;
        ok = PwTColourGate(c, cf, PwTTune.x) > 0.05;
    }
    return !ok;
}

// Rule 3's mask on Accumulate (t1, R8_UINT: PSLinkFail, rule 2's verdict of every texel); a null view otherwise.
Texture2D<uint> PwTLinkFail : register(t1);

// Rule 3: how many of the 25 texels of the 5x5 neighbourhood of `p` (itself included, clamped to the host's motion
// sub-rect, as the marker's PwTMarkerReason) fail rule 2.
uint PwTLinkVote(int2 p)
{
    int2 lo = int2(PwTMotionRect.xy);
    int2 hi = int2(PwTMotionRect.xy + PwTMotionRect.zw) - 1;
    uint failing = 0;
    [loop] for (int oy = -2; oy <= 2; ++oy)
        [loop] for (int ox = -2; ox <= 2; ++ox)
            if (PwTLinkFail.Load(int3(clamp(p + int2(ox, oy), lo, hi), 0)) != 0) ++failing;
    return failing;
}

// Runs over the whole motion texture. Output in motion-texture pixels (already scaled).
float2 PSAccumulate(PwFullscreenVertex input) : SV_Target0
{
    float2 mv;
    float2 acc = PwTChainUntested(input.position.xy, mv);
    if (PwTLinkTested(input.position.xy))
    {
        // DebugTemporalChainRule (PwTSky.z on Accumulate): 0 today, 1 never restart, 2 colour needs a depth hint,
        // 3 rule 2 + 5x5 vote (LinkFail mask at t1), 4 depth only. See phase 3 step 1 (MOTION_ATTRIBUTION.md).
        const int rule = (int) PwTSky.z;
        bool fail = rule != 1 && PwTLinkFails(input.position.xy, acc, rule);
        if (rule == 3 && fail) fail = PwTLinkVote(int2(input.position.xy)) >= 8;
        if (fail) acc = mv;
    }
    return acc;
}

// DebugTemporalChainRule 3, before Accumulate with the same inputs: rule 2's verdict of this texel's own untested
// chain (1 restart, 0 keep; 0 where the link is not tested), for PwTLinkVote.
uint PSLinkFail(PwFullscreenVertex input) : SV_Target0
{
    float2 mv;
    float2 acc = PwTChainUntested(input.position.xy, mv);
    return PwTLinkTested(input.position.xy) && PwTLinkFails(input.position.xy, acc, 2) ? 1u : 0u;
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
//
// Why a motion-texture texel (position: its centre) with the chain `acc` gets that marker, the first test that
// fails: 0 none (the chain is kept), 1 the chain ends off screen, 2 the depth test, 3 the colour gate, 4 a
// non-finite chain. `colour` says when the colour gate may mark: 0 always (today), 1 never, 2 only where the
// depth mismatch is at least the tolerance (half the depth test's threshold).
uint PwTMarkerTest(float2 position, float2 acc, uint colour)
{
    if (any(!isfinite(acc))) return 4;
    float2 nativePixel = (position - PwTMotionRect.xy) / PwTMotionRect.zw * PwTNative.xy;
    float2 qN = nativePixel + acc * PwTNative.xy / PwTMotionRect.zw;
    if (any(qN < 0.0) || any(qN >= PwTNative.xy)) return 1;
    float dc = PwTDepth.Load(int3(PwTRectTexel(nativePixel, PwTDepthRect), 0));
    float mismatch = PwTDepthMismatchAround(PwTExpectedDepth(dc, nativePixel), qN);
    if (mismatch > 2.0 * PwTParams.w) return 2;
    if (PwTTune.x > 0.0 && colour != 1 && (colour == 0 || mismatch >= PwTParams.w))
    {
        float3 c = PwTLoadFrame(PwTColor, int3(PwTRectTexel(nativePixel, PwTColorRect), 0)).rgb;
        float3 cf = PwTLoadFrame(PwTColorF, int3(PwTRectTexel(qN, PwTColorRect), 0)).rgb;
        if (PwTColourGate(c, cf, PwTTune.x) <= 0.05) return 3;
    }
    return 0;
}

// Rules 3/4 in two passes: MarkerFail stores every texel's base verdict (PwTMarkerTest, 0..4) once, here at t1, and
// ModelMotion / MarkerReason (PwTParams.x = 1) read their own and their neighbours' from it instead of running up to 26
// tests per failing texel. PwTParams.x = 0 (rules 0-2, or no MarkerFail binary): every test inline. Same decisions.
Texture2D<uint> PwTMarkerFail : register(t1);

uint PwTMarkerColour()
{
    uint rule = (uint) PwTParams.z;
    return rule > 2 ? rule - 2 : rule;
}

// The base verdict of the texel n (its centre, its own chain) under the rule's colour test.
uint PwTMarkerBase(int2 n, uint colour)
{
    uint base;
    [branch] if (PwTParams.x > 0.5) base = PwTMarkerFail.Load(int3(n, 0));
    else base = PwTMarkerTest(float2(n) + 0.5, PwTAccPrev.Load(int3(n, 0)), colour);
    return base;
}

// The marker's final verdict under DebugTemporalMarkerRule (PwTParams.z on these two passes): 0 today's rule;
// 1 the colour gate never marks; 2 it marks only with a depth hint (see PwTMarkerTest); 3 and 4 = 1 and 2, and
// a failing texel is marked only where at least PwTParams.y (DebugTemporalMarkerNeighbours, 0 = 12) of the 25
// texels of its 5x5 neighbourhood (itself included, clamped to the motion sub-rect) fail the same test - off
// screen and non-finite chains are always marked. PSModelMotion and the diagnostic PSMarkerReason
// (DebugTemporalDumpAcc) share it, so the reason plane reports the decision.
uint PwTMarkerReason(float2 position, float2 acc)
{
    uint rule = (uint) PwTParams.z;
    uint colour = PwTMarkerColour();
    int2 p = int2(position);
    uint reason;
    [branch] if (rule > 2 && PwTParams.x > 0.5) reason = PwTMarkerFail.Load(int3(p, 0));
    else reason = PwTMarkerTest(position, acc, colour);
    if (rule < 3 || reason == 0 || reason == 1 || reason == 4) return reason;
    // The host's motion sub-rect, not the texture: with dynamic resolution the texels past it are not this frame's and
    // would test as off screen. Both routes (mask and inline) read through this clamp.
    int2 lo = int2(PwTMotionRect.xy);
    int2 hi = int2(PwTMotionRect.xy + PwTMotionRect.zw) - 1;
    uint failing = 0;
    [loop] for (int oy = -2; oy <= 2; ++oy)
        [loop] for (int ox = -2; ox <= 2; ++ox)
            if (PwTMarkerBase(clamp(p + int2(ox, oy), lo, hi), colour) != 0) ++failing;
    uint need = PwTParams.y > 0.5 ? (uint) PwTParams.y : 12u;
    return failing >= need ? reason : 0;
}

float2 PSModelMotion(PwFullscreenVertex input) : SV_Target0
{
    float2 acc = PwTAccPrev.Load(int3(int2(input.position.xy), 0));
    return PwTMarkerReason(input.position.xy, acc) != 0 ? float2(4.0 * PwTMotionTex.x, 0.0) : acc;
}

uint PSMarkerReason(PwFullscreenVertex input) : SV_Target0
{
    return PwTMarkerReason(input.position.xy, PwTAccPrev.Load(int3(int2(input.position.xy), 0)));
}

// Rules 3/4, first of the two passes: the base verdict of this texel.
uint PSMarkerFail(PwFullscreenVertex input) : SV_Target0
{
    return PwTMarkerTest(input.position.xy, PwTAccPrev.Load(int3(int2(input.position.xy), 0)), PwTMarkerColour());
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
    // (render-resolution, jittered guides: see PwTDepthMismatchAround), through the previous frame's own sub-rect.
    float2 g = PwTToRect(before, PW_T_DEPTHF_RECT) - 0.5;
    int2 base = int2(floor(g));
    int2 lo = int2(PW_T_DEPTHF_RECT.xy);
    int2 hi = int2(PW_T_DEPTHF_RECT.xy + PW_T_DEPTHF_RECT.zw) - 1;
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
        float2 there = (float2(bestTexel) + 0.5 - PW_T_DEPTHF_RECT.xy) / PW_T_DEPTHF_RECT.zw * PwTNative.xy;
        float4 was = PwTExpect.Load(int3(PwTRectTexel(there, PwTMotionRect), 0));
        return float4(was.w > 0.5 ? asfloat(0x7fc00000u) : was.x, 1.0, !isfinite(was.z) ? dc : was.z, 0.0);
    }
    // Hidden in the previous frame: what is known about it (x) ends here, but how deep it lay in the
    // residual's frame (z) is geometry, and the neighbours of the previous frame at this depth lend it.
    float2 beforeG = PwTToRect(before, PW_T_DEPTHF_RECT);
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
            float2 there = (float2(texel) + 0.5 - PW_T_DEPTHF_RECT.xy) / PW_T_DEPTHF_RECT.zw * PwTNative.xy;
            float z = PwTParams.y < 0.5 ? d : PwTExpect.Load(int3(PwTRectTexel(there, PwTMotionRect), 0)).z;
            if (!isfinite(z)) continue;
            sum += z; n += 1.0;
        }
        if (n > 0.5) return float4(asfloat(0x7fc00000u), 0.0, sum / n, 0.0);
    }
    return float4(asfloat(0x7fc00000u), 0.0, dc, 0.0);
}
#endif

