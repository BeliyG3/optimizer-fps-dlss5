#include "fullscreen.hlsli"
#include "ofps_common.hlsli"
#include "ofps_pack.hlsli"

PwFullscreenVertex VSMain(uint vertexId : SV_VertexID) { return PwFullscreenVS(vertexId); }

struct OfpsPackedOutput
{
    float4 color : SV_Target0;
    float depth : SV_Target1;
    float2 motion : SV_Target2;
    float confidence : SV_Target3;
};

OfpsPackedOutput PSMain(PwFullscreenVertex input)
{
    OfpsPackedOutput output;
    OfpsPackTexel(input.position.xy, output.color, output.depth, output.motion, output.confidence);
    return output;
}
