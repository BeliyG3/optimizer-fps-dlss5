// The sky field (2026.10.2, TemporalSkyField): what a carried frame adds to the flat sky.
//
// A sky pixel that entered from the screen edge, or that an object uncovered after the last full pass, has no
// source in the residual's frame. Its chain still ends somewhere inside that frame: on a featureless sky the
// vectors are zero or arbitrary, and the depth and colour tests pass sky against sky. It took a foreign piece of
// the model's sky edit, which is smooth but differs across the screen, and in a fast turn the pieces formed pale
// patches and mosaics (seen on bench9). A carried frame's pixel whose CURRENT depth is exactly
// the far plane adds this field at its own screen position instead: the residual averaged over the far-plane
// pixels of the pass, on the low-res grid, with the cells that hold no sky filled by push-pull. Sampled at the
// screen position it cannot turn wrong chain ends into patches; during motion its tone is slightly misplaced, which
// on a low-frequency sky edit does not show. The policy is applied where the frame is formed, at native resolution
// (PSCompose and its grid variant, CSReproject's direct route); the reprojection and its acceptance are untouched.
//
// The field speaks only for the sky the pass saw. When a carried frame shows much more sky than that (a reveal: the
// camera leaves a hall, a doorway opens), the pass's few sky blocks, often a strip the model darkened beside the wall
// it borders, would be spread over all the newly revealed sky (bench12, frames 884/885: 7.5 dB below the ordinary
// carry). So the field is weighed by the two shares of sky (PwTSkyWeight): the pass's (the field's support) against
// this frame's (PwTSkyNow, t21: CSSkyNow's lattice over the current depth, pulled to 1x1 by CSSkyPull). A ratio up to
// 0.35 keeps the ordinary carry, from 0.60 the field is used whole, in between the two additions blend. A turn on the
// spot keeps the ratio near 1.
//
// PwTSky.x is the host's far plane: 1 (standard Z) or 0 (reversed Z). Only an exact match counts and a non-finite
// depth never does; a host with linear depth should switch the setting off. PwTSkyField (t20) is the finest filled
// level: rgb the far-plane mean residual in the residual's own (encoded) domain, a the pass's sky support (the
// coarsest level's coverage, the same in every texel). Below 2 % support, or with a null view (the setting off, a
// menu frame, no field built), nothing changes. Built in core/temporal/sky_field.cpp: CSSkyDown, CSSkyPull up to
// 1x1, then CSSkyPush back down; on each carried frame that uses it, CSSkyNow and CSSkyPull up to 1x1.

bool PwTIsFarPlane(float depth)
{
    return isfinite(depth) && depth == PwTSky.x;
}

// A pixel of this frame (native position) the sky policy applies to: far plane, and a field with support.
bool PwTSkyPixel(float2 nativePixel)
{
    if (!(PwTSkyField.Load(int3(0, 0, 0)).a >= 0.02)) return false;
    return PwTIsFarPlane(PwTDepth.Load(int3(PwTRectTexel(nativePixel, PwTDepthRect), 0)));
}

// How much of the field a carried frame's sky takes: 0 the ordinary carry, 1 the field alone. The pass's share of sky
// (the field's support, at least 2 % wherever this is asked) against this frame's (t21): their ratio from 0.35 (0) to
// 0.60 (1), linear between. A frame without a sky sample on the lattice (share 0) has nothing to compare and takes the
// field whole.
float PwTSkyWeight()
{
    const float modelShare = PwTSkyField.Load(int3(0, 0, 0)).a;
    const float nowShare = PwTSkyNow.Load(int3(0, 0, 0)).a;
    return saturate((modelShare / max(nowShare, 1e-6) - 0.35) * 4.0); // * 4 = / (0.60 - 0.35)
}

// Where a carried frame's final addition is formed: for a sky pixel the field's addition, bilinear at its screen
// position, and the weight it takes (PwTSkyWeight); 0 and no value for any other pixel. The caller mixes the value
// into the addition it formed as before (PwTSkyMix), adds that to the encoded colour, then decodes and limits as before.
float PwTSkyAt(float2 nativePixel, out float3 value)
{
    value = float3(0.0, 0.0, 0.0);
    if (!PwTSkyPixel(nativePixel)) return 0.0;
    const float3 f = PwTSkyField.SampleLevel(PwTLinearClamp, nativePixel * PwTNative.zw, 0.0).rgb;
    if (any(!isfinite(f))) return 0.0;
    value = f;
    return PwTSkyWeight();
}

// A pixel's addition: the one formed as before at weight 0 and the field's at weight 1, bit for bit; a blend between.
// A non-finite addition never enters a blend, the field stands in for it.
float3 PwTSkyMix(float3 addition, float3 sky, float weight)
{
    if (weight <= 0.0) return addition;
    if (weight >= 1.0 || any(!isfinite(addition))) return sky;
    return lerp(addition, sky, weight);
}

// Sample k of a finest-level texel's lattice: the centre of the k-th 4x4 sub-block of its 16x16 native block.
float2 PwTSkyLattice(float2 texelCentre, int k)
{
    const float2 block = PwTNative.xy / PwTFill.yz;
    const float2 origin = (texelCentre - 0.5) * block;
    return min(floor(origin + (float2(k & 3, k >> 2) + 0.5) * block * 0.25), PwTNative.xy - 1.0);
}

// CSSkyDown: one texel of the finest level, over the same block as the low-res residual's texel. A 4x4 lattice of
// native pixels (the centres of the block's 4x4 sub-blocks), each read by point: the depth of the pass (t6, the
// snapshot RecordResidual has just taken) through the guide's sub-rect, and the residual (t2). A sample counts when
// its depth is the far plane and both values are finite; rgb = their mean, a = their share.
float4 PwTSkyDown(float2 texelCentre)
{
    float3 sum = 0.0;
    float n = 0.0;
    [unroll] for (int k = 0; k < 16; ++k)
    {
        const float2 pixel = PwTSkyLattice(texelCentre, k);
        const float depth = PwTDepthF.Load(int3(PwTRectTexel(pixel + 0.5, PW_T_DEPTHF_RECT), 0));
        const float3 r = PwTResidual.Load(int3(int2(pixel), 0)).rgb;
        if (!PwTIsFarPlane(depth) || any(!isfinite(r))) continue;
        sum += r;
        n += 1.0;
    }
    return float4(n > 0.0 ? sum / n : float3(0.0, 0.0, 0.0), n / 16.0);
}

// CSSkyNow: this frame's share of sky on CSSkyDown's lattice, from the CURRENT depth (t5) through its sub-rect; a is
// the share of the 16 samples at the far plane, rgb is unused (0). CSSkyPull takes it to 1x1, read as PwTSkyNow.
float4 PwTSkyNowDown(float2 texelCentre)
{
    float n = 0.0;
    [unroll] for (int k = 0; k < 16; ++k)
    {
        const float2 pixel = PwTSkyLattice(texelCentre, k);
        if (PwTIsFarPlane(PwTDepth.Load(int3(PwTRectTexel(pixel + 0.5, PwTDepthRect), 0)))) n += 1.0;
    }
    return float4(0.0, 0.0, 0.0, n / 16.0);
}

// CSSkyPull: one texel of the next coarser level from the up to 2x2 texels of this one (t2; odd sizes round up). Each
// child weighs by the finest-level cells it stands for: PwTParams.x of them per side, fewer on the last row and column
// of an odd-sized grid (PwTFill.yz, the finest level's size). Weighted equally, the half-empty edge texels of an odd
// size counted as whole ones, and one row of sky on a 240x135 grid (0.74 %) came out as 12.5 % at 1x1. rgb is the
// coverage-weighted mean, a the share of sky over the cells the texel stands for; a block without sky stays (0, 0).
float4 PwTSkyPull(int2 texel)
{
    uint w, h;
    PwTResidual.GetDimensions(w, h);
    const float span = PwTParams.x;
    float3 sum = 0.0;
    float cover = 0.0, cells = 0.0;
    [unroll] for (int k = 0; k < 4; ++k)
    {
        const int2 t = texel * 2 + int2(k & 1, k >> 1);
        if (t.x >= int(w) || t.y >= int(h)) continue;
        const float2 extent = min((float2(t) + 1.0) * span, PwTFill.yz) - float2(t) * span;
        const float area = max(extent.x, 0.0) * max(extent.y, 0.0);
        const float4 f = PwTResidual.Load(int3(t, 0));
        sum += f.rgb * (f.a * area);
        cover += f.a * area;
        cells += area;
    }
    return float4(cover > 0.0 ? sum / cover : float3(0.0, 0.0, 0.0), cells > 0.0 ? cover / cells : 0.0);
}

// CSSkyPush: one texel of a filled level from its own pulled texel (t2) and the filled coarser level (t1, bilinear at
// the same place): its own mean where at least half of it is sky, else lerp(coarser, own, coverage / 0.5). a carries
// the sky support down: the coarsest level's coverage, read from the coarser level's first texel.
float4 PwTSkyPush(float2 texelCentre)
{
    uint cw, ch;
    PwTFresh.GetDimensions(cw, ch);
    const float4 own = PwTResidual.Load(int3(int2(texelCentre), 0));
    const float3 coarser = PwTFresh.SampleLevel(PwTLinearClamp, texelCentre * 0.5 / float2(cw, ch), 0.0).rgb;
    const float3 rgb = own.a >= 0.5 ? own.rgb : lerp(coarser, own.rgb, own.a / 0.5);
    return float4(rgb, PwTFresh.Load(int3(0, 0, 0)).a);
}
