// --render raster --ao game: CSAoTemporal, the temporal filter after CSAoBlur (its 2x2 game variant) (raster_ao.cpp). Each pixel follows the
// prepass's motion (current-to-previous pixels) into the previous frame's history and blends it with this frame's
// blurred AO: out = lerp(history, current, AoHistoryWeight(valid, rejected, alpha)), --ao-alpha (0.1) for a usable
// history sample, 1 without one. A sample is rejected off screen, or where its stored view depth differs from this pixel's by more
// than 10 %. The history (R16G16_FLOAT: the AO, the view depth for the next frame's test) ping-pongs between two
// textures; the scene pass reads this frame's as aoTexture (t4 space2, the red channel).
#include "frame.hlsli"
#include "raster_ao_math.hlsli"
// As raster_ao.hlsl's, plus whether the other history texture holds the previous frame (AoHistoryUsable), --ao-alpha.
cbuffer RasterAoPass : register(b1) { float aoRadius; float aoStrength; uint aoReverse; uint aoFrame; uint aoHistoryValid; float aoAlpha; };
Texture2D<float> sceneDepth : register(t0);  // the scene pass's depth buffer (R32), drawn by the prepass
Texture2D<float> aoCurrent : register(t3);   // CSAoBlur's output, this frame
Texture2D<float2> aoMotion : register(t4);   // the prepass's motion, current-to-previous pixels
Texture2D<float2> aoHistory : register(t5);  // the previous frame's history: AO, view depth (0 on the sky)
RWTexture2D<float2> aoHistoryOut : register(u2);
SamplerState historySampler : register(s0);  // bilinear, clamped
[numthreads(8,8,1)]
void CSAoTemporal(uint3 dispatch : SV_DispatchThreadID)
{
    uint2 pixel=dispatch.xy; if(pixel.x>=width || pixel.y>=height) return;
    float z=sceneDepth[pixel];
    if(z==(aoReverse!=0 ? 0.0 : 1.0)) { aoHistoryOut[pixel]=float2(1,0); return; } // the sky: unoccluded, never a match
    float depth=LinearDepth(z,aoReverse!=0), current=aoCurrent[pixel];
    float2 previous=float2(pixel)+0.5+aoMotion[pixel];
    bool rejected=!all(isfinite(previous)) || any(previous<0) || previous.x>width || previous.y>height;
    float2 history=float2(1,0);
    if(!rejected) {
        // Bilinear: a footprint across a depth edge blends the depths and fails the test, which keeps edges clean.
        history=aoHistory.SampleLevel(historySampler,previous/float2(width,height),0);
        rejected=!(abs(history.y-depth)<=0.1*depth) || !isfinite(history.x); // NaN fails too
    }
    float weight=AoHistoryWeight(aoHistoryValid!=0,rejected,aoAlpha);
    // weight 1 takes the current AO alone, so a texture never written (after create or resize) cannot leak in.
    float ao=weight>=1 ? current : lerp(history.x,current,weight);
    aoHistoryOut[pixel]=float2(saturate(ao),depth);
}
