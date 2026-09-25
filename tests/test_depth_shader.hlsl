// Executes the production HLSL on D3D11 WARP, including PSExpect and history encoding.
#define PW_T_EXPECT 1
#define PW_T_HISTORY 1
#include "../core/shaders/temporal.hlsl"
cbuffer TestCases : register(b1) { float4 Pairs[4]; };
RWStructuredBuffer<float4> Result : register(u0);
[numthreads(1, 1, 1)]
void TestDepth(uint3 id : SV_DispatchThreadID)
{
    float d = PwTDepth.Load(int3(0, 0, 0));
    PwFullscreenVertex p = (PwFullscreenVertex)0;
    p.position = float4(0.5, 0.5, 0, 1);
    p.uv = float2(0.5, 0.5);
    Result[0] = float4(PwTDepthMismatch(Pairs[0].x, Pairs[0].y), PwTDepthMismatch(Pairs[0].z, Pairs[0].w),
        PwTDepthMismatch(Pairs[1].x, Pairs[1].y), PwTDepthMismatch(Pairs[1].z, Pairs[1].w));
    Result[1] = float4(PwTDepthMismatch(Pairs[2].x, Pairs[2].y), PwTDepthMismatch(Pairs[2].z, Pairs[2].w),
        PwTDepthMismatch(d, Pairs[3].x), PwTDepthMismatch(d, Pairs[3].y));
    Result[2] = PSExpect(p);
    float expected = PwTExpectedDepth(d, p.position.xy);
    Result[3] = float4(expected, PwTDepthMismatchAround(expected, p.position.xy),
        PwTDepthMismatchAround(d, p.position.xy), 0);
    Result[4] = PSHistory(p).second;
    float3 older;
    float weight = PwTFromOlderPasses(p.position.xy, d, float3(0, 0, 0), 1, older);
    Result[5] = float4(older, weight);
}
