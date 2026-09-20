#include "haze.hlsli"
RWTexture2D<float4> colour : register(u0);
RWTexture2D<float> depth : register(u1);
RWTexture2D<float2> motion : register(u2);
RWTexture2D<float4> normalRoughness : register(u3);
RWTexture2D<float4> diffuseAlbedo : register(u4);
RWTexture2D<float4> specularAlbedo : register(u5);
RWTexture2D<float> specHitDistance : register(u6);
RWTexture2D<float4> accumulated : register(u7);
RWStructuredBuffer<uint2> pickResult : register(u8);
RWTexture2D<float4> accumulatedInput : register(u9);

float2 Pixel(float4 clip) { return (clip.xy/clip.w*float2(0.5,-0.5)+0.5)*float2(width,height); }
float3 Radiance(Surface primary, float3 direction, inout uint rng, out float specularDistance)
{
    // Complementary source-proposal MIS restores BSDF-sampled lamp reflections.
    // Near-mirror samples have a dominant BSDF PDF and retain essentially full emission.
    specularDistance=0;
    Surface s=primary; float3 incoming=direction, throughput=1;
    float3 result=s.emission;
    for(uint depthIndex=0;;++depthIndex) {
        bool last=depthIndex>=bounces;
        result+=throughput*Direct(s,-incoming,rng,!last);
        if(last) break;
        bool specular;
        float3 bounce=SampleBsdf(s,-incoming,rng,specular);
        float pdf=BsdfPdf(s,-incoming,bounce);
        if(pdf<=0) break;
        throughput*=Bsdf(s,-incoming,bounce)*saturate(dot(s.normal,bounce))/pdf;
        Intersection hit=Trace(s.position+s.geometric*0.001,bounce,0.001,1e30,false);
        if(depthIndex==0 && specular && hit.hit) specularDistance=hit.distance;
        if(!hit.hit) { result+=throughput*Sky(bounce); break; }
        Surface next=GetSurface(hit,bounce);
        TriMaterial lamp=materials[hit.triangleIndex];
        float3 lightNormal=normalize(cross(vertices[hit.triangleIndex*3+1].pos-vertices[hit.triangleIndex*3].pos,
            vertices[hit.triangleIndex*3+2].pos-vertices[hit.triangleIndex*3].pos));
        float distance=length(next.position-s.position);
        // Dynamic emissive triangles have no NEE proposal, so their BSDF emission keeps weight one.
        float lampPdf=lightPower>0 && lamp.lampGroup!=0xffffffffu ? Luminance(Emission(lamp))/lightPower*distance*distance/max(abs(dot(lightNormal,-bounce)),1e-8) : 0;
        result+=throughput*next.emission*(1-LampMis(lampPdf,pdf));
        s=next; incoming=bounce;
    }
    return result;
}
[numthreads(8,8,1)]
void CS(uint3 dispatch : SV_DispatchThreadID)
{
    uint2 pixel=dispatch.xy; if(pixel.x>=width || pixel.y>=height) return;
    // A positive projection jitter moves the image right/down, so ray positions subtract it.
    float2 ndc=(float2(pixel)+0.5-float2(jitterX,jitterY))/float2(width,height)*2-1;
    float3 direction=normalize(cameraForward+cameraRight*(ndc.x*aspect*tanHalf)-cameraUp*(ndc.y*tanHalf));
    float forwardCos=dot(direction,cameraForward);
    Intersection hit=Trace(eye,direction,0.1/forwardCos,300/forwardCos,false);
    if(pickX>=0 && all(pixel==uint2(pickX,pickY)))
        pickResult[0]=hit.hit ? uint2(hit.triangleIndex,materials[hit.triangleIndex].lampGroup) : uint2(0xffffffffu,0xffffffffu);
    float3 sum=0;
    depth[pixel]=reverseDepth==2 ? -300.0 : reverseDepth==3 ? 300.0 : reverseDepth!=0 ? 0 : 1; motion[pixel]=0; normalRoughness[pixel]=0;
    diffuseAlbedo[pixel]=0; specularAlbedo[pixel]=0; specHitDistance[pixel]=0;
    if(hit.hit) {
        Surface s=GetSurface(hit,direction);
        float4 now=mul(currentVP,float4(s.position,1)), old=mul(previousVP,float4(s.previous,1));
        // Clip w is positive view-space Z for the bench left-handed projection.
        depth[pixel]=reverseDepth==2 ? -now.w : reverseDepth==3 ? now.w : saturate(now.z/now.w);
        // First Light convention: raw * (-width/2,+height/2) = current-to-previous pixels.
        float2 delta=old.w>1e-5 ? Pixel(old)-Pixel(now) : float2(0,0);
        motion[pixel]=motionNdc!=0 ? delta/float2(-0.5*width,0.5*height) : delta;
        normalRoughness[pixel]=float4(s.normal,s.roughness);
        diffuseAlbedo[pixel]=float4(s.albedo,1);
        float nv=dot(s.normal,-direction);
        specularAlbedo[pixel]=float4(SpecularGuideF0(s.roughness,nv,s.f0.r),
            SpecularGuideF0(s.roughness,nv,s.f0.g),SpecularGuideF0(s.roughness,nv,s.f0.b),1);
        for(uint sample=0;sample<spp;++sample) {
            uint rng=Hash(pixel.x+pixel.y*width)^Hash(frameIndex+0x9e3779b9u)^Hash(sample+0x85ebca6bu);
            float hitDistance;
            float3 value=max(Radiance(s,direction,rng,hitDistance)*exposure,0);
            // With explicit multisampling, retain sample zero's actual segment, never an average.
            if(sample==0) specHitDistance[pixel]=min(hitDistance,65504);
            if(!all(isfinite(value))) value=0;
            sum+=value;
        }
        sum/=spp;
    } else {
        sum=max(Sky(direction)*exposure,0);
        if(!all(isfinite(sum))) sum=0;
    }
    uint fogRng=Hash(pixel.x+pixel.y*width)^Hash(frameIndex+0x73a9b19du);
    float fogDistance=min(hit.hit ? hit.distance : 300/forwardCos,300);
    sum=sum*MediumTransmittance(hazeDensity,fogDistance)+Haze(direction,fogDistance,fogRng)*exposure;
    if(!all(isfinite(sum))) sum=0;
    sum*=min(1,firefly/max(Luminance(sum),1e-8));
    // Retain the noisy frame independently of the optional static-camera reference.
    colour[pixel]=float4(sum,1);
    if(accumulation==0) accumulated[pixel]=float4(sum,1);
    else if(accumulation<accumulationLimit) accumulated[pixel]=float4(lerp(accumulated[pixel].rgb,sum,1.0/(accumulation+1)),1);
    // Keep the reference in FP32, but supply the FP16 colour format expected by NGX.
    if(feedAccumulation!=0) accumulatedInput[pixel]=accumulated[pixel];
}
