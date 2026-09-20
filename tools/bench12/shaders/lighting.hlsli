#ifndef PW_LIGHTING_HLSLI
#define PW_LIGHTING_HLSLI
#include "bsdf.hlsli"
#include "reservoir.hlsli"
#include "specular_guide.hlsli"
#include "sampling_math.hlsli"
bool Visible(Surface s, float3 direction, float distance)
{
    return !Trace(s.position+s.geometric*0.001,direction,0.001,distance,true).hit;
}
float3 Direct(Surface s, float3 outgoing, inout uint rng, bool continuation)
{
    float3 result=0;
    float cosine=lerp(sunCos,1,Random(rng)), phi=2*PI*Random(rng), sine=sqrt(max(0,1-cosine*cosine));
    float3 direction=BasisSample(float3(sine*cos(phi),sine*sin(phi),cosine),-sunTravel);
    float nl=saturate(dot(s.normal,direction));
    if(nl>0 && sunStrength>0 && Visible(s,direction,1e30)) result+=Bsdf(s,outgoing,direction)*nl*sunStrength;
    if(lightCount==0 || lightPower<=0) return result;
    // RIS in area measure. Zero-target candidates still count toward M. Only the selected
    // sample is shadowed; M=1 reduces exactly to ordinary power-CDF next-event estimation.
    LampReservoir reservoir=(LampReservoir)0;
    float selectedDistance=0;
    float3 selectedContribution=0, selectedDirection=0;
    for(uint candidate=0;candidate<lightCandidates;++candidate) {
        float select=Random(rng); uint low=0, high=lightCount-1;
        while(low<high) { uint middle=(low+high)/2; if(select<lights[middle].cdf) high=middle; else low=middle+1; }
        EmissiveTriangle light=lights[low]; float r=sqrt(Random(rng)), v=Random(rng);
        float2 bary=float2(r*(1-v),r*v);
        if(!Accept(light.triangleIndex,bary)) continue;
        float3 a=vertices[light.triangleIndex*3].pos, b=vertices[light.triangleIndex*3+1].pos, c=vertices[light.triangleIndex*3+2].pos;
        float3 delta=a*(1-r)+b*bary.x+c*bary.y-s.position;
        float distance=length(delta); if(distance<0.004) continue;
        direction=delta/distance; nl=saturate(dot(s.normal,direction));
        float lampCos=abs(dot(normalize(cross(b-a,c-a)),-direction));
        TriMaterial lamp=materials[light.triangleIndex];
        float3 emission=Emission(lamp)*BaseTexture(lamp,TriangleUv(light.triangleIndex,bary),length(s.position+delta-eye)).rgb;
        float3 contribution=emission*Bsdf(s,outgoing,direction)*nl*lampCos/(distance*distance);
        float target=Luminance(contribution), sourcePdf=light.power/(lightPower*light.area);
        if(UpdateLampReservoir(reservoir,target,sourcePdf,Random(rng))) {
            float lampPdf=sourcePdf*distance*distance/max(lampCos,1e-8);
            selectedContribution=contribution*(continuation ? LampMis(lampPdf,BsdfPdf(s,outgoing,direction)) : 1);
            selectedDirection=direction; selectedDistance=distance;
        }
    }
    if(reservoir.selectedTarget>0 && Visible(s,selectedDirection,selectedDistance-0.003))
        result+=selectedContribution*LampNormalization(reservoir,lightCandidates);
    return result;
}
#endif
