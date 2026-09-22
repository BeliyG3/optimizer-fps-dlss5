#include "fullscreen.hlsli"
#include "peripheral_warp_common.hlsli"
#include "peripheral_warp_unpack.hlsli"

PwFullscreenVertex VSMain(uint vertexId : SV_VertexID) { return PwFullscreenVS(vertexId); }

struct PwNativeOutput
{
    float4 color : SV_Target0;
    float depth : SV_Target1;
    float2 motion : SV_Target2;
    float confidence : SV_Target3;
};

PwNativeOutput PSMain(PwFullscreenVertex input)
{
    PwNativeOutput output;
    PwUnpackTexel(input.position.xy, output.color, output.depth, output.motion, output.confidence);
    return output;
}
