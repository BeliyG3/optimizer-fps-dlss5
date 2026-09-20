#include "frame.hlsli"
#include "display_math.hlsli"
Texture2D<float4> colour : register(t0);
Texture2D<float> depth : register(t1);
Texture2D<float2> motion : register(t2);
Texture2D<float4> normalRoughness : register(t3);
Texture2D<float4> diffuseAlbedo : register(t4);
Texture2D<float4> specularAlbedo : register(t5);
Texture2D<float> specHitDistance : register(t6);
Texture2D<float4> accumulated : register(t7);
Texture2D<float4> bloomSmall : register(t8);
Texture2D<float4> bloomMedium : register(t9);
Texture2D<float4> bloomLarge : register(t10);
Texture2D<float4> displayExposure : register(t11);
SamplerState linearClamp : register(s0);
struct Blit { float4 position : SV_Position; float2 uv : TEXCOORD; };
Blit VS(uint id : SV_VertexID)
{
    Blit o; o.uv=float2((id<<1)&2,id&2); o.position=float4(o.uv*float2(2,-2)+float2(-1,1),0,1); return o;
}
float3 Srgb(float3 c)
{
    return float3(c.x<=0.0031308 ? c.x*12.92 : 1.055*pow(c.x,1/2.4)-0.055,
                  c.y<=0.0031308 ? c.y*12.92 : 1.055*pow(c.y,1/2.4)-0.055,
                  c.z<=0.0031308 ? c.z*12.92 : 1.055*pow(c.z,1/2.4)-0.055);
}
float4 PS(Blit i) : SV_Target
{
    float3 result=0;
    if(viewMode==1) result=Srgb(saturate(diffuseAlbedo.SampleLevel(linearClamp,i.uv,0).rgb));
    else if(viewMode==6) result=normalRoughness.SampleLevel(linearClamp,i.uv,0).www;
    else if(viewMode==7) result=Srgb(saturate(specularAlbedo.SampleLevel(linearClamp,i.uv,0).rgb));
    else if(viewMode==2) result=normalRoughness.SampleLevel(linearClamp,i.uv,0).xyz*0.5+0.5;
    else if(viewMode==3) result=depth.SampleLevel(linearClamp,i.uv,0).xxx;
    else if(viewMode==4) result=float3(saturate(0.5+motion.SampleLevel(linearClamp,i.uv,0)/32),0.5);
    else {
        float3 hdr=viewMode==5 ? accumulated.SampleLevel(linearClamp,i.uv,0).rgb : colour.SampleLevel(linearClamp,i.uv,0).rgb;
        hdr+=bloom*(bloomSmall.SampleLevel(linearClamp,i.uv,0).rgb+
            bloomMedium.SampleLevel(linearClamp,i.uv,0).rgb+bloomLarge.SampleLevel(linearClamp,i.uv,0).rgb)/3;
        hdr=max(hdr,0)*displayExposure.Load(int3(0,0,0)).r;
        float3 mapped=neutralTonemap!=0 ? hdr/(1+hdr) : float3(Filmic(hdr.r),Filmic(hdr.g),Filmic(hdr.b));
        result=Srgb(saturate(mapped));
    }
    return float4(saturate(result),1);
}
