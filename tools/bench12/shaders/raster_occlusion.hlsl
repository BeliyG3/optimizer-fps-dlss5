// --render raster: Hi-Z occlusion of small instances in the camera passes (raster_occlusion.h; hiz_math.h is the same test
// in C++). The first camera pass draws the occluders (the flattened triangles and the large instances), then:
// CSHiZ: one Hi-Z level, each texel the farthest depth of the 2x2 texels below it (clamped at the edge); level 0 reads the
//   depth buffer. reverse: the farthest depth is the smallest.
// CSOcclusion: one thread per small candidate the camera's frustum kept: tests its world box against the Hi-Z and appends it
//   to its run's visible list (args[run].InstanceCount, the run's StartInstanceLocation is its first entry) or, occluded,
//   to the culled list (args[runs + run], for --cull-debug), and writes its flag (1 occluded) for --cull-check.
#include "frame.hlsli"
cbuffer Pass : register(b1) {
    uint count, levels, hizWidth, hizHeight; // CSHiZ: source width, height, this level's width, height
    uint reverse, runs, fromDepth, padding;
};
Texture2D<float> source : register(t0); // CSHiZ: the level below (or the depth buffer); CSOcclusion: the Hi-Z, all levels
RWTexture2D<float> target : register(u0);
struct Candidate { uint instance, run; };
struct Box { float3 lo, hi; };
StructuredBuffer<Candidate> candidates : register(t1);
StructuredBuffer<Box> boxes : register(t2);
RWByteAddressBuffer args : register(u1); // DrawInstanced arguments: vertices, instances, first vertex, first instance
RWStructuredBuffer<uint> visible : register(u2);
RWStructuredBuffer<uint> flags : register(u3);

float Farther(float a, float b) { return reverse!=0 ? min(a,b) : max(a,b); }

[numthreads(8,8,1)]
void CSHiZ(uint3 id : SV_DispatchThreadID)
{
    if(id.x>=hizWidth || id.y>=hizHeight) return;
    uint2 last=uint2(count,levels)-1; // CSHiZ: count, levels = the source's width and height
    uint2 a=min(id.xy*2,last), b=min(id.xy*2+1,last);
    float d=Farther(Farther(source.Load(int3(a.x,a.y,0)),source.Load(int3(b.x,a.y,0))),Farther(source.Load(int3(a.x,b.y,0)),source.Load(int3(b.x,b.y,0))));
    target[id.xy]=d;
}
bool Occluded(Box box)
{
    float minX=1e30f, minY=1e30f, maxX=-1e30f, maxY=-1e30f, nearest=reverse!=0 ? 0.0f : 1.0f;
    [unroll] for(uint k=0;k<8;++k) {
        float4 p=float4(k&1 ? box.hi.x : box.lo.x,k&2 ? box.hi.y : box.lo.y,k&4 ? box.hi.z : box.lo.z,1);
        float4 c=mul(currentVP,p);
        if(c.w<0.1f) return false; // in front of the near plane: visible
        float x=(c.x/c.w*0.5f+0.5f)*float(width), y=(0.5f-c.y/c.w*0.5f)*float(height), depth=c.z/c.w;
        minX=min(minX,x); maxX=max(maxX,x); minY=min(minY,y); maxY=max(maxY,y);
        nearest=reverse!=0 ? max(nearest,depth) : min(nearest,depth);
    }
    // One pixel of margin for the jitter (the depth was drawn jittered, at most half a pixel).
    float x0=max(floor(minX-1),0.0f), x1=min(floor(maxX+1),float(width)-1), y0=max(floor(minY-1),0.0f), y1=min(floor(maxY+1),float(height)-1);
    if(x0>x1 || y0>y1) return false;
    uint tx0=uint(x0)>>1, tx1=uint(x1)>>1, ty0=uint(y0)>>1, ty1=uint(y1)>>1, level=0;
    while((tx1-tx0>1 || ty1-ty0>1) && level+1<levels) { tx0>>=1; tx1>>=1; ty0>>=1; ty1>>=1; ++level; }
    float farthest=reverse!=0 ? 1.0f : 0.0f;
    for(uint y=ty0;y<=ty1;++y) for(uint x=tx0;x<=tx1;++x) farthest=Farther(farthest,source.Load(int3(x,y,level)));
    return reverse!=0 ? nearest<farthest : nearest>farthest;
}
[numthreads(64,1,1)]
void CSOcclusion(uint id : SV_DispatchThreadID)
{
    if(id>=count) return;
    Candidate c=candidates[id];
    bool hidden=Occluded(boxes[c.instance]);
    uint run=hidden ? runs+c.run : c.run, slot;
    args.InterlockedAdd(run*16+4,1,slot);
    visible[args.Load(run*16+12)+slot]=c.instance;
    flags[id]=hidden ? 1 : 0;
}
