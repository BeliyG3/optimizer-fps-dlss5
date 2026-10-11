// --local-exposure: the bilateral grid of local_exposure.hlsli, built from the display source and this frame's auto
// exposure (Grid), then blurred (Blur) into the grid the present pass slices. Each cell holds (sum of log2 luminance,
// pixel count) / (LE_CELL * LE_CELL), so filtering and blurring weigh by pixels and the base is sum / count.
#include "local_exposure.hlsli"
cbuffer Grid : register(b0) { uint2 sourceSize, gridSize; };
Texture2D<float4> source : register(t0);
Texture2D<float4> exposureScale : register(t1); // Display's 1x1 adapted scale (1 without auto exposure)
Texture3D<float2> gridIn : register(t0, space1);
RWTexture3D<float2> grid : register(u0);
groupshared uint sums[LE_SLICES], counts[LE_SLICES];
// One group per cell: 16x16 threads, 4x4 pixels each. Sums in fixed point (1/256 stop above LE_LOG_LOW): at most
// 4096 x 16 x 256 per slice, well inside a uint.
[numthreads(16,16,1)]
void Grid(uint3 cell : SV_GroupID, uint3 thread : SV_GroupThreadID, uint index : SV_GroupIndex)
{
    if(index<LE_SLICES) { sums[index]=0; counts[index]=0; }
    GroupMemoryBarrierWithGroupSync();
    float scale=exposureScale.Load(int3(0,0,0)).r;
    for(uint y=0;y<4;++y) for(uint x=0;x<4;++x) {
        uint2 p=cell.xy*LE_CELL+thread.xy*4+uint2(x,y);
        if(any(p>=sourceSize)) continue;
        float l=clamp(log2(max(LeLuminance(max(source.Load(int3(p,0)).rgb,0))*scale,1e-10)),LE_LOG_LOW,LE_LOG_HIGH);
        uint slice=LocalExposureSlice(l);
        InterlockedAdd(sums[slice],uint((l-LE_LOG_LOW)*256+0.5));
        InterlockedAdd(counts[slice],1u);
    }
    GroupMemoryBarrierWithGroupSync();
    if(index<LE_SLICES) {
        float n=float(counts[index]);
        grid[uint3(cell.xy,index)]=float2(float(sums[index])/256+LE_LOG_LOW*n,n)/(LE_CELL*LE_CELL);
    }
}
// A 5x5 binomial blur across cells and a 3-tap one across slices, edges clamped.
[numthreads(4,4,4)]
void Blur(uint3 id : SV_DispatchThreadID)
{
    if(any(id>=uint3(gridSize,LE_SLICES))) return;
    const float w5[5]={1,4,6,4,1}, w3[3]={1,2,1};
    float2 sum=0; float total=0;
    for(int dz=-1;dz<=1;++dz) for(int dy=-2;dy<=2;++dy) for(int dx=-2;dx<=2;++dx) {
        int3 q=clamp(int3(id)+int3(dx,dy,dz),int3(0,0,0),int3(gridSize,LE_SLICES)-1);
        float w=w5[dx+2]*w5[dy+2]*w3[dz+1];
        sum+=gridIn.Load(int4(q,0))*w; total+=w;
    }
    grid[id]=sum/total;
}
