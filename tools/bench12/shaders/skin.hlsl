// GPU skinning (gpu_skinning.h, skin_layout.h): one thread per item vertex deforms it at this frame and at the previous one
// (morph targets, the blended palette of its node, the --wind sway) and writes position, previous position and normal to
// every corner of the dynamic range that uses it. UVs are written once at load. skin_math.h is the same math in C++.
struct SkinSource { float3 position; float3 normal; uint2 joints; float4 weights; };
struct SkinItem { uint source, firstVertex, vertexCount, palette, targets, targetCount, weights, wind; };
struct PaletteRows { float4 r[3]; };
cbuffer SkinPass : register(b0) {
    uint vertexCount, dynamicBase, previousPalette, previousWeights; // dynamicBase: the first dynamic vertex of the scene buffer
    float windStrength; uint groupsX; uint2 padding;
    float4 windNow, windBefore; // sin and cos of omega t, sin and cos of 2.7 omega t (skin_math.h WindPhase)
};
StructuredBuffer<SkinSource> sources : register(t0);
StructuredBuffer<SkinItem> items : register(t1);
StructuredBuffer<uint> vertexItem : register(t2);
StructuredBuffer<uint> offsets : register(t3);
StructuredBuffer<uint> outputs : register(t4);
StructuredBuffer<float3> targets : register(t5);
StructuredBuffer<float> windWeights : register(t6);
StructuredBuffer<PaletteRows> palette : register(t7);
StructuredBuffer<float> morphWeights : register(t8);
RWByteAddressBuffer vertices : register(u0); // TriVertex: pos, prevPos, normal, uv (44 bytes)

static const float WindDirX=0.8f, WindDirZ=0.6f, WindWave=0.35f, WindAmplitude=0.08f, WindFlutter=0.03f; // wind.h

float3 WindOffset(float4 phase, float3 position, float weight)
{
    if(weight==0 || windStrength==0) return 0;
    float p=position.x*WindDirX+position.z*WindDirZ, a=WindWave*p, b=1.3f*WindWave*p;
    float sway=WindAmplitude*(phase.x*cos(a)+phase.y*sin(a))+WindFlutter*(phase.z*cos(b)+phase.w*sin(b));
    float s=windStrength*weight*sway;
    return float3(WindDirX*s,0,WindDirZ*s);
}
float3 Morph(SkinItem item, uint v, float3 p, uint weightBase)
{
    for(uint k=0;k<item.targetCount;++k) {
        float w=morphWeights[weightBase+item.weights+k];
        if(w!=0) p+=targets[item.targets+k*item.vertexCount+v]*w;
    }
    return p;
}
// The blended rows of the vertex's palette entries (a weighted sum, as Affine3x4::Add) and their total weight.
void Blend(SkinItem item, SkinSource s, uint paletteBase, out float4 rows[3], out float total)
{
    uint entries[4]={s.joints.x&0xffffu,s.joints.x>>16,s.joints.y&0xffffu,s.joints.y>>16};
    rows[0]=0; rows[1]=0; rows[2]=0; total=0;
    [unroll] for(uint k=0;k<4;++k) {
        float w=s.weights[k];
        if(w==0) continue;
        PaletteRows m=palette[paletteBase+item.palette+entries[k]];
        rows[0]+=m.r[0]*w; rows[1]+=m.r[1]*w; rows[2]+=m.r[2]*w; total+=w;
    }
}
float3 Point(float4 rows[3], float3 p) { return float3(dot(rows[0].xyz,p)+rows[0].w,dot(rows[1].xyz,p)+rows[1].w,dot(rows[2].xyz,p)+rows[2].w); }
float3 Direction(float4 rows[3], float3 p) { return float3(dot(rows[0].xyz,p),dot(rows[1].xyz,p),dot(rows[2].xyz,p)); }

[numthreads(64,1,1)]
void CS(uint3 group : SV_GroupID, uint thread : SV_GroupIndex)
{
    uint g=(group.y*groupsX+group.x)*64+thread;
    if(g>=vertexCount) return;
    SkinItem item=items[vertexItem[g]];
    uint v=g-item.firstVertex;
    SkinSource s=sources[item.source+v];
    float4 rows[3]; float total;
    Blend(item,s,0,rows,total);
    float3 position=Point(rows,Morph(item,v,s.position,0))*(1/total);
    float3 n=Direction(rows,s.normal);
    float3 normal=dot(n,n)>0 ? n*rsqrt(dot(n,n)) : 0;
    Blend(item,s,previousPalette,rows,total);
    float3 previous=Point(rows,Morph(item,v,s.position,previousWeights))*(1/total);
    if(item.wind!=0) {
        float weight=windWeights[g];
        position+=WindOffset(windNow,position,weight); previous+=WindOffset(windBefore,previous,weight);
    }
    for(uint k=offsets[g];k<offsets[g+1];++k) {
        uint address=(dynamicBase+outputs[k])*44;
        vertices.Store3(address,asuint(position)); vertices.Store3(address+12,asuint(previous)); vertices.Store3(address+24,asuint(normal));
    }
}
