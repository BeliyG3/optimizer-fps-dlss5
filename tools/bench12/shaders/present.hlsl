#include "frame.hlsli"
#include "display_math.hlsli"
#include "tonemap_ue.hlsli"
#include "local_exposure.hlsli"
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
Texture3D<float2> localGrid : register(t12);   // --local-exposure: the blurred bilateral grid (LocalExposure)
Texture2D<float4> localBlur : register(t13);   // a coarse pyramid level: alpha is the mean ln luminance before exposure
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
// --local-exposure: the factor for the pixel of colour c at uv (scale: the auto exposure), UE5's LocalExposure.
float LocalExposureAt(float3 c, float2 uv, float scale)
{
    float pixel=log2(max(LeLuminance(c)*scale,1e-10));
    uint w,h,gw,gh,gd; colour.GetDimensions(w,h); localGrid.GetDimensions(gw,gh,gd);
    float3 at=float3(uv*float2(w,h)/(LE_CELL*float2(gw,gh)),LocalExposureDepth(clamp(pixel,LE_LOG_LOW,LE_LOG_HIGH)));
    float2 cell=localGrid.SampleLevel(linearClamp,at,0);
    float blurred=localBlur.SampleLevel(linearClamp,uv,0).a*1.44269504+log2(scale);
    float bilateral=cell.y>1e-5 ? cell.x/cell.y : blurred; // no pixel of about this brightness near: the wide blur
    return LocalExposureFactor(pixel,bilateral,blurred,localHighlights,localShadows,localDetail,localBlend);
}
float4 PS(Blit i) : SV_Target
{
    float3 result=0;
    // --view vegmask: the raster foliage class (foliage_class.h) in the motion target, class*50 in red; point-read, never blended.
    if(viewMode==8u) { uint w,h; motion.GetDimensions(w,h); return float4(motion.Load(int3(i.uv*float2(w,h),0)).x*50.0/255.0,0,0,1); }
    if(viewMode==1) result=Srgb(saturate(diffuseAlbedo.SampleLevel(linearClamp,i.uv,0).rgb));
    else if(viewMode==6) result=normalRoughness.SampleLevel(linearClamp,i.uv,0).www;
    else if(viewMode==7) result=Srgb(saturate(specularAlbedo.SampleLevel(linearClamp,i.uv,0).rgb));
    else if(viewMode==2) result=normalRoughness.SampleLevel(linearClamp,i.uv,0).xyz*0.5+0.5;
    else if(viewMode==3) result=depth.SampleLevel(linearClamp,i.uv,0).xxx;
    else if(viewMode==4) result=float3(saturate(0.5+motion.SampleLevel(linearClamp,i.uv,0)/32),0.5);
    else {
        float3 hdr=viewMode==5 ? accumulated.SampleLevel(linearClamp,i.uv,0).rgb : colour.SampleLevel(linearClamp,i.uv,0).rgb;
        float scale=displayExposure.Load(int3(0,0,0)).r;
        if(localExposure!=0) hdr=max(hdr,0)*LocalExposureAt(max(hdr,0),i.uv,scale); // before the bloom, as UE
        hdr+=bloom*(bloomSmall.SampleLevel(linearClamp,i.uv,0).rgb+
            bloomMedium.SampleLevel(linearClamp,i.uv,0).rgb+bloomLarge.SampleLevel(linearClamp,i.uv,0).rgb)/3;
        hdr=max(hdr,0)*scale;
        // 2 (--tonemap none): no curve, the colour is clipped at 1 and sRGB-encoded as it is. 3: UE's filmic curve.
        UeFilm film; film.slope=filmSlope; film.toe=filmToe; film.shoulder=filmShoulder; film.blackClip=filmBlackClip; film.whiteClip=filmWhiteClip;
        float3 mapped=neutralTonemap==3 ? UeFilmToneMap(hdr,film) : neutralTonemap==2 ? hdr : neutralTonemap!=0 ? hdr/(1+hdr) :
            float3(Filmic(hdr.r),Filmic(hdr.g),Filmic(hdr.b));
        result=Srgb(saturate(mapped));
    }
    return float4(saturate(result),1);
}
