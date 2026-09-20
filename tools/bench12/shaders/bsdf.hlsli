#ifndef PW_BSDF_HLSLI
#define PW_BSDF_HLSLI
#include "scene.hlsli"
float3 Fresnel(float3 f0, float cosine) { return f0+(1-f0)*pow(1-saturate(cosine),5); }
float SpecProbability(Surface s, float3 outgoing) { return clamp(Luminance(Fresnel(s.f0,dot(s.normal,outgoing))),0.1,0.9); }
float Distribution(float nh, float a2) { float d=nh*nh*(a2-1)+1; return a2/(PI*d*d); }
float Smith(float cosine, float a2) { return 2*cosine/max(cosine+sqrt(a2+(1-a2)*cosine*cosine),1e-7); }
float3 Bsdf(Surface s, float3 outgoing, float3 incoming)
{
    float nl=dot(s.normal,incoming), nv=dot(s.normal,outgoing);
    if(nl<=0 || nv<=0 || dot(s.geometric,incoming)<=0) return 0;
    float3 halfVector=normalize(outgoing+incoming); float3 f=Fresnel(s.f0,dot(outgoing,halfVector));
    float a2=pow(s.roughness,4);
    float3 spec=Distribution(saturate(dot(s.normal,halfVector)),a2)*Smith(nl,a2)*Smith(nv,a2)*f/max(4*nl*nv,1e-7);
    return s.albedo*(1-f)/PI+spec;
}
float BsdfPdf(Surface s, float3 outgoing, float3 incoming)
{
    float nl=dot(s.normal,incoming); if(nl<=0 || dot(s.geometric,incoming)<=0) return 0;
    float p=SpecProbability(s,outgoing); float3 halfVector=normalize(outgoing+incoming);
    float nh=saturate(dot(s.normal,halfVector));
    float spec=Distribution(nh,pow(s.roughness,4))*nh/max(4*abs(dot(outgoing,halfVector)),1e-7);
    return (1-p)*nl/PI+p*spec;
}
float3 SampleBsdf(Surface s, float3 outgoing, inout uint rng, out bool specular)
{
    float choose=Random(rng), u=Random(rng), phi=2*PI*Random(rng);
    specular=choose<SpecProbability(s,outgoing);
    if(specular) {
        float a2=pow(s.roughness,4), cosine=sqrt((1-u)/(1+(a2-1)*u));
        float sine=sqrt(max(0,1-cosine*cosine));
        float3 halfVector=BasisSample(float3(sine*cos(phi),sine*sin(phi),cosine),s.normal);
        return reflect(-outgoing,halfVector);
    }
    return BasisSample(float3(sqrt(u)*cos(phi),sqrt(u)*sin(phi),sqrt(1-u)),s.normal);
}
#endif
