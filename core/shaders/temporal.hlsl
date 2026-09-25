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
    // 9 float4 (36 root constants).
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


#include "temporal_mapping.hlsli"
#include "temporal_residual.hlsli"
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
#include "temporal_reproject.hlsli"

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
