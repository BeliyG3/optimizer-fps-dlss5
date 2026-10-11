// DebugTemporalCarryChainRule (phase 3 steps 1b/1c): the main chain and the carry chain in one pass each
// (core/temporal/carry_chain.cpp), so a link both chains share is tested once and a chain both share is registered once.
// The main chain keeps DebugTemporalChainRule (PwTSky.z, 0..4), the carry chain follows DebugTemporalCarryChainRule
// (PwTSky.w, 1..6). Rules 0-4 are PwTLinkFails' (temporal_residual.hlsli), and two tell a change of a whole area
// (a reflection, a light) from the speckled changes of swaying foliage:
//   5 coherent colour: a colour fail restarts the chain only where at least PW_TEMPORAL_COHERENT_VOTES of the 25 texels
//     of its 5x5 neighbourhood fail by colour too, whatever the depth; a depth fail restarts it as always;
//   6 rule 5, and a restart wherever last frame's Refine moved the main chain coherently over the 5x5 neighbourhood
//     (PwTRefineDisagrees): the picture moved there in a way the game's vectors did not.
// Rules 3, 5 and 6 vote over the 5x5 neighbourhood: AccumulatePair tests its tile and a 2-texel apron into groupshared
// memory (the apron only where a texel of the tile votes) and votes from there. Compute only (temporal_cs.hlsl).

// t2: the carry chain - its previous value on AccumulatePair (the main chain's when it adopts that), its accumulated
// value on RefinePair. t8, AccumulatePair only: the main chain as last frame's Accumulate left it, before its Refine (t4
// holds the refined one); PwTParams.z = 1 when it is there (rule 6), 0 and a null view otherwise.
Texture2D<float2> PwTAccCarry : register(t2);
Texture2D<float2> PwTAccUnrefined : register(t8);

// What the link test finds for the chain `acc` of the texel at `position`, for every rule at once, with the same tests as
// PwTLinkFails (which the single chain's passes keep as they are): bit 0 the depth test fails (a mismatch above twice the
// tolerance; NaN fails), bit 1 the colour gate fails (tested where the depth test holds), bit 2 rule 2 fails (depth, or
// colour with a mismatch of at least the tolerance). 0 where the chain's end is off the frame.
// (Macros, not static consts: a static const global changes fxc's code for unrelated entry points, ReprojectGrid's.)
#define kPwTLinkDepth 1u
#define kPwTLinkColour 2u
#define kPwTLinkRule2 4u
// Both chains' findings in one value (PwTPairFindings): the carry chain's in bits 0-2, the main chain's in bits 4-6.
#define kPwTPairMainShift 4u
uint PwTLinkFindings(float2 position, float2 acc)
{
    float2 nativePixel = (position - PwTMotionRect.xy) / PwTMotionRect.zw * PwTNative.xy;
    float2 qN = nativePixel + acc * PwTNative.xy / PwTMotionRect.zw;
    bool inside = all(qN >= 0.0) && all(qN < PwTNative.xy);
    if (!inside) return 0;
    float dc = PwTDepth.Load(int3(PwTRectTexel(nativePixel, PwTDepthRect), 0));
    float mismatch = PwTDepthMismatchAround(PwTExpectedDepth(dc, nativePixel), qN);
    if (!(mismatch <= 2.0 * PwTParams.w)) return kPwTLinkDepth | kPwTLinkRule2;
    if (!(PwTTune.x > 0.0)) return 0;
    float3 c = PwTLoadFrame(PwTColor, int3(PwTRectTexel(nativePixel, PwTColorRect), 0)).rgb;
    float3 cf = PwTLoadFrame(PwTColorF, int3(PwTRectTexel(qN, PwTColorRect), 0)).rgb;
    if (PwTColourGate(c, cf, PwTTune.x) > 0.05) return 0;
    return mismatch >= PwTParams.w ? kPwTLinkColour | kPwTLinkRule2 : kPwTLinkColour;
}

// Whether rule `rule` restarts a link with these findings before any vote: 0 on depth or colour; 2 and 3 on rule 2's
// verdict; 4 and 5/6 on depth (5/6's colour fails go to the vote); 1 never.
bool PwTLinkRuleFails(uint findings, int rule)
{
    if (rule == 0) return (findings & (kPwTLinkDepth | kPwTLinkColour)) != 0;
    if (rule == 2 || rule == 3) return (findings & kPwTLinkRule2) != 0;
    return rule != 1 && (findings & kPwTLinkDepth) != 0;
}
// Rule 6: last frame's Refine moved the main chain one way over the 5x5 neighbourhood of `p` (clamped to the motion
// sub-rect): the mean correction is at least PW_TEMPORAL_COHERENT_SHIFT native pixels long and at least
// PW_TEMPORAL_COHERENT_SHARE of the mean correction's length. Random corrections (noise, swaying blades) cancel out.
// Native pixels, not the chain's motion-texture ones: Refine moves the chain at most 0.8 native pixels a frame, so on a
// motion texture coarser than the output (an upscaler's) a threshold in its own pixels could never be reached.
bool PwTRefineDisagrees(int2 p)
{
    if (PwTParams.z < 0.5) return false; // last frame left no unrefined chain
    int2 lo = int2(PwTMotionRect.xy);
    int2 hi = int2(PwTMotionRect.xy + PwTMotionRect.zw) - 1;
    float2 toNative = PwTNative.xy / PwTMotionRect.zw;
    float2 sum = float2(0.0, 0.0);
    float lengths = 0.0;
    [loop] for (int oy = -2; oy <= 2; ++oy)
        [loop] for (int ox = -2; ox <= 2; ++ox)
        {
            int3 t = int3(clamp(p + int2(ox, oy), lo, hi), 0);
            float2 d = (PwTAccPrev.Load(t) - PwTAccUnrefined.Load(t)) * toNative;
            if (all(isfinite(d))) { sum += d; lengths += length(d); }
        }
    float shift = length(sum) / 25.0;
    return shift >= PW_TEMPORAL_COHERENT_SHIFT && shift >= PW_TEMPORAL_COHERENT_SHARE * lengths / 25.0;
}

// The findings of both chains of the texel at `position`: the carry chain's in bits 0-2, the main chain's in bits 4-6. A
// link the chains share is tested once.
uint PwTPairFindings(float2 position, float2 main, float2 carry)
{
    uint mainF = PwTLinkFindings(position, main);
    uint carryF = mainF;
    [branch] if (!all(carry == main)) carryF = PwTLinkFindings(position, carry);
    return carryF | (mainF << kPwTPairMainShift);
}

struct PwTChainPair { float2 main; float2 carry; };

// AccumulatePair's votes (rules 3, 5, 6) from groupshared memory: every texel's findings of the group's tile and of an
// apron of 2 texels, each texel tested once, then PwTAccumulatePair's 5x5 votes over them. A slot holds the findings of
// its position clamped to the motion sub-rect, so a vote at the edge counts what the clamped neighbourhood holds.
#define kPwTApronW (PW_TEMPORAL_PAIR_THREADS_X + 4)
#define kPwTApronH (PW_TEMPORAL_PAIR_THREADS_Y + 4)
groupshared uint gPwTFindings[kPwTApronW * kPwTApronH];
groupshared uint gPwTVoteWanted; // 1 when a texel of the tile votes: only then is the apron tested

// The findings a vote reads at `q`, clamped to the motion sub-rect: of the main chain (bits 4-6) when it votes, of the
// carry chain (bits 0-2) when it does; 0 where the link is not tested.
uint PwTApronFindings(int2 q, bool mainVotes, bool carryVotes)
{
    const float2 position = float2(clamp(q, int2(PwTMotionRect.xy), int2(PwTMotionRect.xy + PwTMotionRect.zw) - 1)) + 0.5;
    if (!PwTLinkTested(position)) return 0u;
    const float2 mv = PwTFrameVector(position);
    const float2 main = mainVotes ? PwTChainContinued(PwTAccPrev, position, mv) : float2(0.0, 0.0);
    if (!carryVotes) return PwTLinkFindings(position, main) << kPwTPairMainShift;
    const float2 carry = PwTChainContinued(PwTAccCarry, position, mv);
    return mainVotes ? PwTPairFindings(position, main, carry) : PwTLinkFindings(position, carry);
}

// How many of the 25 slots around the texel at `local` (its tile position) have `bit`.
uint PwTApronVote(int2 local, uint bit)
{
    uint count = 0;
    [unroll] for (int oy = 0; oy <= 4; ++oy)
        [unroll] for (int ox = 0; ox <= 4; ++ox)
            if ((gPwTFindings[(local.y + oy) * kPwTApronW + local.x + ox] & bit) != 0) ++count;
    return count;
}

// AccumulatePair: PSAccumulate for both chains, each by its own rule, for the texel at `p` (`local` in the tile at
// `origin`; `own` false past the target, where the thread only helps fill the apron). A link the chains share is tested
// once. Every thread of the group calls it: it synchronises the group.
PwTChainPair PwTAccumulatePair(int2 p, int2 local, int2 origin, uint index, bool own)
{
    const int mainRule = (int) PwTSky.z, carryRule = (int) PwTSky.w;
    const bool mainVotes = mainRule == 3, carryVotes = carryRule == 3 || carryRule >= 5;
    if (index == 0) gPwTVoteWanted = 0u;
    GroupMemoryBarrierWithGroupSync();
    const float2 position = float2(p) + 0.5;
    PwTChainPair o;
    o.main = o.carry = float2(0.0, 0.0);
    float2 mv = o.main;
    uint found = 0u;
    bool tested = false, mainFail = false, carryFail = false, vote = false;
    if (own)
    {
        mv = PwTFrameVector(position);
        o.main = PwTChainContinued(PwTAccPrev, position, mv);
        o.carry = PwTChainContinued(PwTAccCarry, position, mv);
        tested = PwTLinkTested(position);
        if (tested) found = PwTPairFindings(position, o.main, o.carry);
        mainFail = PwTLinkRuleFails((found >> kPwTPairMainShift) & 7u, mainRule);
        carryFail = PwTLinkRuleFails(found & 7u, carryRule);
        vote = (mainVotes && mainFail) || (carryRule == 3 && carryFail) ||
               (carryRule >= 5 && !carryFail && (found & kPwTLinkColour) != 0);
    }
    gPwTFindings[(local.y + 2) * kPwTApronW + local.x + 2] = found;
    if (vote) InterlockedOr(gPwTVoteWanted, 1u);
    GroupMemoryBarrierWithGroupSync();
    if (gPwTVoteWanted != 0u)
    {
        const int2 lo = int2(PwTMotionRect.xy), hi = int2(PwTMotionRect.xy + PwTMotionRect.zw) - 1;
        const int2 tile = int2(PW_TEMPORAL_PAIR_THREADS_X, PW_TEMPORAL_PAIR_THREADS_Y);
        [loop] for (uint s = index; s < kPwTApronW * kPwTApronH; s += PW_TEMPORAL_PAIR_THREADS_X * PW_TEMPORAL_PAIR_THREADS_Y)
        {
            const int2 a = int2(s % kPwTApronW, s / kPwTApronW), q = origin + a - 2;
            // a texel of the tile inside the sub-rect holds its own findings already
            if (all(a >= 2) && all(a < tile + 2) && all(q >= lo) && all(q <= hi)) continue;
            gPwTFindings[s] = PwTApronFindings(q, mainVotes, carryVotes);
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if (tested)
    {
        if (mainRule == 3 && mainFail) mainFail = PwTApronVote(local, kPwTLinkRule2 << kPwTPairMainShift) >= 8;
        if (carryRule == 3 && carryFail) carryFail = PwTApronVote(local, kPwTLinkRule2) >= 8;
        if (carryRule >= 5 && !carryFail && (found & kPwTLinkColour) != 0)
            carryFail = PwTApronVote(local, kPwTLinkColour) >= PW_TEMPORAL_COHERENT_VOTES;
        if (carryRule == 6 && !carryFail) carryFail = PwTRefineDisagrees(p);
        if (mainFail) o.main = mv;
        if (carryFail) o.carry = mv;
    }
    return o;
}
// RefinePair: PSRefine for both chains; where the carry chain equals the main one it takes the main one's result.
PwTChainPair PwTRefinePair(float2 position)
{
    const int3 t = int3(int2(position), 0);
    const float2 main = PwTAccPrev.Load(t), carry = PwTAccCarry.Load(t);
    PwTChainPair o;
    o.main = PwTRefineChain(position, main);
    o.carry = o.main;
    [branch] if (!all(carry == main)) o.carry = PwTRefineChain(position, carry);
    return o;
}
