// Block-constant motion vectors made smooth within each surface (the 64-bit Feed host's optical flow).
//
// The host's optical flow engine measures one vector per grid x grid cell and hands the model a
// full-resolution field in which every pixel carries its cell's vector. Inside a surface the field
// therefore steps at every cell boundary, and the model, which moves its history along these
// vectors, leaves a lattice of those steps in the picture - squares on grass, walls and text,
// measured on the DX9 bench as a periodic structure at 4 and 8 pixels.
//
// Each pixel here takes the four cells around it with bilinear weights, each weight multiplied by
// how well the depth at the cell's centre matches the pixel's own depth. Inside one surface the four
// agree and the field becomes smooth; across an object's edge the cells on the other side lose their
// weight, so a pixel keeps the vector of its own surface and nothing is invented between two
// surfaces (the reason the host expands the field by nearest cell in the first place).
//
// Only block-constant pixels are touched: a pixel whose vector is not exactly its cell centre's is a
// real per-pixel field (a game's own vectors, the host's shader estimate) and passes through as is.

Texture2D<float2>   PwMsMotion : register(t0);
Texture2D<float>    PwMsDepth  : register(t1);
RWTexture2D<float2> PwMsOut    : register(u0);

cbuffer PwMsConstants : register(b0)
{
    uint4  PwMsDims;   // motion width, height, depth width, height
    float4 PwMsParams; // cell size in pixels (the grid), relative depth tolerance, -, -
};

// How far apart two depth samples are, as a fraction, whatever the buffer's encoding (the same
// measure the temporal carry uses, see PwTDepthMismatch in temporal.hlsl).
float PwMsDepthMismatch(float a, float b)
{
    if (!isfinite(a) || !isfinite(b)) return 1e9;
    float mismatch = abs(a - b) / max(max(abs(a), abs(b)), 1e-6);
    if (a >= 0.0 && a <= 1.0 && b >= 0.0 && b <= 1.0)
        mismatch = max(mismatch, abs(a - b) / max(max(1.0 - a, 1.0 - b), 1e-6));
    return mismatch;
}

float PwMsDepthAt(uint2 motionPixel)
{
    const float2 scale = float2(PwMsDims.zw) / float2(PwMsDims.xy);
    const uint2 p = min(uint2((float2(motionPixel) + 0.5) * scale), PwMsDims.zw - 1);
    return PwMsDepth.Load(int3(p, 0));
}

[numthreads(16, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    const uint2 dims = PwMsDims.xy;
    if (any(id.xy >= dims)) return;
    const float2 own = PwMsMotion.Load(int3(id.xy, 0));

    // The host maps pixel x to cell floor(x * cells / width); the cell's centre pixel is the inverse.
    const float grid = PwMsParams.x;
    const uint2 cells = (dims + uint(grid) - 1) / uint(grid);
    const float2 toCell = float2(cells) / float2(dims);
    const uint2 ownCell = min(uint2(float2(id.xy) * toCell), cells - 1);
    const uint2 ownCentre = min(uint2((float2(ownCell) + 0.5) / toCell), dims - 1);
    const float2 centreVector = PwMsMotion.Load(int3(ownCentre, 0));
    if (any(centreVector != own) || any(!isfinite(own)))
    {
        PwMsOut[id.xy] = own;
        return;
    }

    const float depthHere = PwMsDepthAt(id.xy);
    const float tolerance = PwMsParams.y;
    const float2 g = (float2(id.xy) + 0.5) * toCell - 0.5; // cell centres sit at integers
    const int2 base = int2(floor(g));
    const float2 t = g - float2(base);
    float2 sum = 0.0;
    float weightSum = 0.0;
    [unroll] for (int k = 0; k < 4; ++k)
    {
        const int2 o = int2(k & 1, k >> 1);
        const uint2 cell = uint2(clamp(base + o, int2(0, 0), int2(cells) - 1));
        const uint2 centre = min(uint2((float2(cell) + 0.5) / toCell), dims - 1);
        const float2 v = PwMsMotion.Load(int3(centre, 0));
        const float bilinear = (o.x == 0 ? 1.0 - t.x : t.x) * (o.y == 0 ? 1.0 - t.y : t.y);
        const float same = 1.0 - smoothstep(tolerance, 2.0 * tolerance, PwMsDepthMismatch(depthHere, PwMsDepthAt(centre)));
        if (!all(isfinite(v))) continue; // NaN * 0 is still NaN: a bad cell is skipped, not weighted away
        const float w = bilinear * same;
        sum += v * w;
        weightSum += w;
    }
    PwMsOut[id.xy] = weightSum > 1e-4 ? sum / weightSum : own;
}
