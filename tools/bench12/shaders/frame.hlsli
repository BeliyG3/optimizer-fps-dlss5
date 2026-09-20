#ifndef PW_FRAME_HLSLI
#define PW_FRAME_HLSLI
cbuffer Frame : register(b0) {
    row_major float4x4 currentVP, previousVP;
    float3 eye; float tanHalf;
    float3 cameraRight; float aspect;
    float3 cameraUp; float exposure;
    float3 cameraForward; float albedoScale;
    float3 sunTravel; float sunStrength;
    uint width, height, frameIndex, spp;
    uint bounces, opaqueTriangles, lightCount, accumulation;
    float jitterX, jitterY, firefly, sunCos;
    float lightPower; uint reverseDepth, viewMode, lightCandidates;
    float hazeDensity, hazeG, bloom; uint neutralTonemap;
    int pickX, pickY; uint accumulationLimit, feedAccumulation;
    uint dynamicBase, dynamicOpaque, motionNdc, animationPadding1;
};
#endif
