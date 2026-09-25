#include "fullscreen.hlsli"
#include "ofps_common.hlsli"

PwFullscreenVertex VSMain(uint vertexId : SV_VertexID)
{
    return PwFullscreenVS(vertexId);
}

float4 PSMain(PwFullscreenVertex input) : SV_Target0
{
    float4 diagnosticColor;
    if (OfpsDiagnosticOutlineColor(input.position.xy, diagnosticColor))
        return diagnosticColor;
    discard;
    return 0.0;
}
