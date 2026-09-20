#ifndef PW_HAZE_HLSLI
#define PW_HAZE_HLSLI
#include "lighting.hlsli"
float3 Haze(float3 direction, float distance, inout uint rng)
{
    if(hazeDensity<=0) return 0;
    float t=MediumDistance(hazeDensity,distance,Random(rng));
    float3 position=eye+direction*t;
    float3 result=0;
    // Single scattering in a finite camera medium (far plane on a miss). The sun
    // enters at the same camera-centred medium boundary; no arbitrary infinite fog.
    float cosine=lerp(sunCos,1,Random(rng)), phi=2*PI*Random(rng);
    float sine=sqrt(max(0,1-cosine*cosine));
    float3 toSun=BasisSample(float3(sine*cos(phi),sine*sin(phi),cosine),-sunTravel);
    float projected=dot(position-eye,toSun);
    float sunDistance=max(0,-projected+sqrt(max(0,300*300-t*t+projected*projected)));
    if(sunStrength>0 && !Trace(position,toSun,0.001,sunDistance,true).hit)
        result+=sunStrength*PhaseHG(dot(direction,toSun),hazeG)*MediumTransmittance(hazeDensity,sunDistance);
    LampReservoir reservoir=(LampReservoir)0;
    float3 selectedContribution=0, selectedDirection=0; float selectedDistance=0;
    for(uint candidate=0;candidate<lightCandidates && lightCount>0 && lightPower>0;++candidate) {
        float select=Random(rng); uint low=0, high=lightCount-1;
        while(low<high) { uint middle=(low+high)/2; if(select<lights[middle].cdf) high=middle; else low=middle+1; }
        EmissiveTriangle light=lights[low]; float r=sqrt(Random(rng)), v=Random(rng);
        float2 bary=float2(r*(1-v),r*v);
        if(!Accept(light.triangleIndex,bary)) continue;
        float3 a=vertices[light.triangleIndex*3].pos, b=vertices[light.triangleIndex*3+1].pos, c=vertices[light.triangleIndex*3+2].pos;
        float3 lampPosition=a*(1-r)+b*bary.x+c*bary.y, delta=lampPosition-position;
        float lengthToLamp=length(delta); if(lengthToLamp<0.004) continue;
        float3 toLamp=delta/lengthToLamp;
        float lampCos=abs(dot(normalize(cross(b-a,c-a)),-toLamp));
        TriMaterial lamp=materials[light.triangleIndex];
        float3 emission=Emission(lamp)*BaseTexture(lamp,TriangleUv(light.triangleIndex,bary),length(lampPosition-eye)).rgb;
        float3 contribution=emission*PhaseHG(dot(direction,toLamp),hazeG)*lampCos/(lengthToLamp*lengthToLamp)
            *MediumTransmittance(hazeDensity,lengthToLamp);
        if(UpdateLampReservoir(reservoir,Luminance(contribution),light.power/(lightPower*light.area),Random(rng))) {
            selectedContribution=contribution; selectedDirection=toLamp; selectedDistance=lengthToLamp;
        }
    }
    if(reservoir.selectedTarget>0 && !Trace(position,selectedDirection,0.001,selectedDistance-0.002,true).hit)
        result+=selectedContribution*LampNormalization(reservoir,lightCandidates);
    // sigma_s*T(t)/pdf(t) = albedo*(1-T(max)), with one distance sample per pixel.
    return result*(0.9*(1-MediumTransmittance(hazeDensity,distance)));
}
#endif
