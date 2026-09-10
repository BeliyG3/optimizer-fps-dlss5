#ifndef PERIPHERAL_WARP_FULLSCREEN_HLSLI
#define PERIPHERAL_WARP_FULLSCREEN_HLSLI

struct PwFullscreenVertex
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

PwFullscreenVertex PwFullscreenVS(uint vertexId : SV_VertexID)
{
    PwFullscreenVertex output;
    output.uv = float2((vertexId << 1) & 2, vertexId & 2);
    output.position = float4(output.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return output;
}

#endif
