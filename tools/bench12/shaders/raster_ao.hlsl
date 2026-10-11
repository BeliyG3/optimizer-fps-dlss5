// --render raster --ao: screen-space ambient occlusion from the prepass's depth and normal (raster_ao.cpp).
// CSAo: GTAO, 8 slice directions with a horizon search of 6 steps on both sides within --ao-radius (projected to
// pixels, at most AoMaxPixels), the cosine-weighted visibility integrated per slice, then raised to --ao-strength. The slices
// turn and the steps shift per pixel in a fixed 4x4 interleaved pattern (AoPattern), which the blur averages out.
// CSAoBlur: a 4x4 box weighted by depth similarity, into the texture the scene pass reads (aoTexture, t4 space2).
// PW_AO_GAME=1 (--ao game, raster_ao_game.cso, raster_ao_blur_game.cso): 2 slice directions of 4 steps turned and stepped by
// per-frame interleaved gradient noise (AoGameNoise), and a 2x2 blur, so per-frame noise survives as in a game that
// relies on TAA; raster_ao_temporal.hlsl then filters it over time.
// View space here: x right, y up, z forward (the camera's basis in the frame constants).
#include "frame.hlsli"
#include "raster_ao_math.hlsli"
cbuffer RasterAoPass : register(b1) { float aoRadius; float aoStrength; uint aoReverse; uint aoFrame; };
Texture2D<float> sceneDepth : register(t0);   // the scene pass's depth buffer (R32), drawn by the prepass
Texture2D<float2> sceneNormal : register(t1); // the prepass's octahedral world normal
Texture2D<float> aoRaw : register(t2);        // CSAo's output, read by CSAoBlur
RWTexture2D<float> aoRawOut : register(u0);
RWTexture2D<float> aoOut : register(u1);
static const float AoPi=3.14159265359;
#if PW_AO_GAME
static const uint AoDirections=2, AoSteps=4;
#else
static const uint AoDirections=8, AoSteps=6;
#endif
// The projected radius's cap, an eighth of the render height (135 px at 1080p, 270 px at 4K): a fixed 64 px cut --ao-radius 1 to
// 14 cm at 4 m in 4K, so props left no contact darkening on the ground or the wall behind them.
float AoMaxPixels() { return max(64.0,0.125*height); }

bool AoSky(float z) { return z==(aoReverse!=0 ? 0.0 : 1.0); }
// The surface a pixel centre shows, from its depth and the jittered pixel ray (as the resolve builds it).
float3 AoViewPosition(float2 pixelCentre, float z)
{
    float2 ndc=(pixelCentre-float2(jitterX,jitterY))/float2(width,height)*2-1;
    return float3(ndc.x*aspect*tanHalf,-ndc.y*tanHalf,1)*LinearDepth(z,aoReverse!=0);
}
// The cosine of one sample's horizon angle (from the view vector), faded toward `low`, the tangent plane's, between
// 60 % and 100 % of the radius; `low` too off screen and on the sky.
float AoHorizon(uint2 pixel, float2 offset, float3 position, float3 view, float low)
{
    float2 at=floor(float2(pixel)+0.5+offset)+0.5;
    if(any(at<0) || at.x>=width || at.y>=height) return low;
    float z=sceneDepth[uint2(at)];
    if(AoSky(z)) return low;
    float3 delta=AoViewPosition(at,z)-position; float gap=length(delta);
    float weight=saturate((aoRadius-gap)/(0.4*aoRadius));
    return lerp(low,dot(delta,view)/max(gap,1e-6),weight);
}
[numthreads(8,8,1)]
void CSAo(uint3 dispatch : SV_DispatchThreadID)
{
    uint2 pixel=dispatch.xy; if(pixel.x>=width || pixel.y>=height) return;
    float z=sceneDepth[pixel];
    if(AoSky(z)) { aoRawOut[pixel]=1; return; }
    float3 position=AoViewPosition(float2(pixel)+0.5,z), view=normalize(-position);
    float3 worldNormal=OctDecode(sceneNormal[pixel]);
    float3 normal=float3(dot(worldNormal,cameraRight),dot(worldNormal,cameraUp),dot(worldNormal,cameraForward));
    float radiusPixels=min(aoRadius*height/(2*tanHalf*position.z),AoMaxPixels());
    if(radiusPixels<1) { aoRawOut[pixel]=1; return; }
#if PW_AO_GAME
    // game: a per-pixel rotation and step offset that change every frame; the 2x2 blur averages them only in part.
    float2 pattern=AoGameNoise(pixel,aoFrame);
#else
    // clean: a fixed per-pixel rotation and step offset (no frame offset), averaged out by CSAoBlur's 4x4 box.
    float2 pattern=AoPattern(pixel,0);
#endif
    float visibility=0;
    [loop] for(uint d=0;d<AoDirections;++d) {
        // The slice through the view vector and this screen direction (y down on screen, up in view space).
        float phi=(d+pattern.x)*AoPi/AoDirections;
        float2 omega=float2(cos(phi),-sin(phi));
        float3 direction=float3(cos(phi),sin(phi),0), ortho=direction-dot(direction,view)*view;
        float3 axis=normalize(cross(ortho,view)), projected=normal-axis*dot(normal,axis);
        float projectedLength=length(projected);
        if(projectedLength<1e-4) continue;
        // n: the projected normal's angle from the view vector, positive toward +omega.
        float cosN=saturate(dot(projected,view)/projectedLength);
        float n=(dot(ortho,projected)<0 ? -1.0 : 1.0)*acos(cosN);
        float low0=cos(n+AoPi/2), low1=cos(n-AoPi/2), horizon0=low0, horizon1=low1;
        [unroll] for(uint k=0;k<AoSteps;++k) {
            // Steps crowd toward the pixel (quadratic), each at least one pixel further than the last.
            float s=(k+pattern.y)/AoSteps;
            float2 offset=omega*max(radiusPixels*s*s,k+1.0);
            horizon0=max(horizon0,AoHorizon(pixel,offset,position,view,low0));
            horizon1=max(horizon1,AoHorizon(pixel,-offset,position,view,low1));
        }
        // A horizon cosine can round past 1 (a sample on the view ray); acos would return NaN.
        float h0=-acos(clamp(horizon1,-1.0,1.0)), h1=acos(clamp(horizon0,-1.0,1.0));
        h0=n+max(h0-n,-AoPi/2); h1=n+min(h1-n,AoPi/2);
        float sinN=sin(n);
        visibility+=projectedLength*0.25*((-cos(2*h0-n)+cosN+2*h0*sinN)+(-cos(2*h1-n)+cosN+2*h1*sinN));
    }
    float ao=pow(max(saturate(visibility/AoDirections),1e-4),aoStrength);
    aoRawOut[pixel]=isfinite(ao) ? ao : 1;
}
[numthreads(8,8,1)]
void CSAoBlur(uint3 dispatch : SV_DispatchThreadID)
{
    uint2 pixel=dispatch.xy; if(pixel.x>=width || pixel.y>=height) return;
    float z=sceneDepth[pixel];
    if(AoSky(z)) { aoOut[pixel]=1; return; }
    float depth=LinearDepth(z,aoReverse!=0), sum=0, weights=0;
#if PW_AO_GAME
    [unroll] for(int y=-1;y<1;++y) [unroll] for(int x=-1;x<1;++x) { // game: 2x2
#else
    [unroll] for(int y=-2;y<2;++y) [unroll] for(int x=-2;x<2;++x) {
#endif
        int2 at=int2(pixel)+int2(x,y);
        if(any(at<0) || at.x>=int(width) || at.y>=int(height)) continue;
        float zq=sceneDepth[at];
        if(AoSky(zq)) continue;
        float weight=exp(-abs(LinearDepth(zq,aoReverse!=0)-depth)/(0.05*depth));
        sum+=weight*aoRaw[at]; weights+=weight;
    }
    aoOut[pixel]=sum/weights; // weights >= 1: the pixel itself
}
