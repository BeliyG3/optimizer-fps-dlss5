#include "fullscreen.hlsli"
#include "peripheral_warp_common.hlsli"

PwFullscreenVertex VSMain(uint vertexId : SV_VertexID)
{
    return PwFullscreenVS(vertexId);
}

float4 PSMain(PwFullscreenVertex input) : SV_Target0
{
    float4 diagnosticColor;
    if (PwDiagnosticOutlineColor(input.position.xy, diagnosticColor))
        return diagnosticColor;
    discard;
    return 0.0;
}
