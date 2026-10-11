// --sky-reflections (raster_reflections.cpp, RASTER_REFLECTIONS.md): the sky's prefiltered cube, built once at start.
// Source: the panorama the path tracer's miss shader and the raster sky read (Sky() in scene.hlsli, through SkyUv) as a
// SkySourceSize cube; Downsample: its mip chain (2x2 boxes); Prefilter: mip m of the reflection cube, GGX-filtered at
// roughness m / (SkyReflectionMips - 1) from the source's mips (sky_reflection_math.hlsli).
#include "sky_uv.hlsli"
cbuffer SkyPass : register(b0) { uint passSize, passMip; }; // the written mip's size and index (raster_reflections.cpp)
Texture2D<float4> environment : register(t0);    // the scene's sky panorama (Scene::environment)
TextureCube<float4> skySource : register(t1);    // the source cube, all mips (Prefilter)
RWTexture2DArray<float4> lower : register(u0);   // Downsample: the finer mip; Source, Prefilter: the written mip
RWTexture2DArray<float4> upper : register(u1);   // Downsample: the coarser mip it writes
SamplerState linearWrap : register(s0);          // as the path tracer's s0
#define PW_PREFILTER_SOURCE(direction,lod) skySource.SampleLevel(linearWrap,direction,lod).rgb
#include "sky_reflection_math.hlsli"

float3 FaceDirection(uint3 id, float2 offset) { return CubeFaceDirection(id.z,(id.x+offset.x)*2.0/passSize-1,(id.y+offset.y)*2.0/passSize-1); }
// The panorama over the source texel: 4x4 bilinear taps (a source texel spans about 4x4 panorama texels of a 4k sky).
[numthreads(8,8,1)]
void Source(uint3 id : SV_DispatchThreadID)
{
    if(id.x>=passSize || id.y>=passSize) return;
    uint w,h; environment.GetDimensions(w,h);
    float3 sum=0;
    [unroll] for(uint y=0;y<4;++y) [unroll] for(uint x=0;x<4;++x)
        sum+=environment.SampleLevel(linearWrap,SkyUv(FaceDirection(id,float2(x+0.5,y+0.5)/4),h),0).rgb;
    lower[id]=float4(sum/16,1);
}
[numthreads(8,8,1)]
void Downsample(uint3 id : SV_DispatchThreadID)
{
    if(id.x>=passSize || id.y>=passSize) return;
    uint3 p=uint3(id.xy*2,id.z);
    upper[id]=(lower[p]+lower[p+uint3(1,0,0)]+lower[p+uint3(0,1,0)]+lower[p+uint3(1,1,0)])*0.25;
}
[numthreads(8,8,1)]
void Prefilter(uint3 id : SV_DispatchThreadID)
{
    if(id.x>=passSize || id.y>=passSize) return;
    float3 n=FaceDirection(id,0.5);
    // Mip 0 is the mirror: the source at its texel's footprint.
    float3 colour=passMip==0 ? skySource.SampleLevel(linearWrap,n,log2(float(SkySourceSize)/passSize)).rgb
                             : PrefilterGgx(n,SkyMipRoughness(passMip),SkyPrefilterSamples);
    lower[id]=float4(colour,1);
}
