#ifndef PW_RASTER_TUBES_HLSLI
#define PW_RASTER_TUBES_HLSLI
// --lamp-tubes (PW_TUBES, raster_tubes.cpp): every emissive tube of a lamp group is a line light (raster_lights.h
// BuildRasterLampTubes); the groups' compact emitters stay rasterLights. Unshadowed, as the point lights. Included by
// raster_common.hlsli. Space 6 is used by no other root: one root SRV and one root constant.
struct RasterLine { float3 a; float radius; float3 b; float twoSided; float3 power; float range; float3 normal; float faceShare;
    float sideShare; float3 side; }; // raster_lights.h
StructuredBuffer<RasterLine> rasterLines : register(t0,space6);
cbuffer RasterTubes : register(b0,space6) { uint rasterLineCount; };

// The mean over the segment a..b of max(n.(x-p), 0) / |x-p|^4, the segment clipped to the horizon of n and the distance to
// its axis held at `radius`. Times m.(p-a) (the same for the whole segment when m is perpendicular to it) it is the
// irradiance at p from a line light of total intensity 1 whose intensity is cos(the angle to m): LineIrradiance in
// raster_lights.cpp. `mid` returns the direction to the middle of the clipped segment (for the diffuse's Fresnel term).
float LineIntegral(float3 p, float3 n, float3 a, float3 b, float radius, out float3 mid)
{
    float3 l0=a-p, l1=b-p; mid=0;
    float d0=dot(n,l0), d1=dot(n,l1), hold=max(radius,1e-4), full=length(b-a);
    if(d0<=0 && d1<=0) return 0;
    if(full<1e-6) { float r2=max(dot(l0,l0),hold*hold); mid=normalize(l0); return d0/(r2*r2); }
    if(d0<0) l0=l0+(l1-l0)*(d0/(d0-d1)); else if(d1<0) l1=l0+(l1-l0)*(d0/(d0-d1));
    mid=normalize(l0+l1);
    // x = p + h + s u (h perpendicular to u): n.(x-p) / r^4 = (n.h + s n.u) / (h^2 + s^2)^2, integrated in closed form.
    float3 u=(b-a)/full;
    float s0=dot(l0,u), s1=dot(l1,u); float3 hv=l0-u*s0;
    float h=max(length(hv),hold), h2=h*h, nh=dot(n,hv), nu=dot(n,u), q0=h2+s0*s0, q1=h2+s1*s1;
    float f0=nh*(s0/(2*h2*q0)+atan(s0/h)/(2*h2*h))-nu/(2*q0), f1=nh*(s1/(2*h2*q1)+atan(s1/h)/(2*h2*h))-nu/(2*q1);
    return (f1-f0)/full;
}
// Bsdf's diffuse lobe, albedo (1 - F) / PI, for light from `incoming`; the same view and geometric tests.
float3 TubeDiffuse(Surface s, float3 outgoing, float3 incoming)
{
    if(dot(s.normal,outgoing)<=0 || dot(s.geometric,incoming)<=0) return 0;
    return s.albedo*(1-Fresnel(s.f0,dot(outgoing,normalize(outgoing+incoming))))/PI;
}
// One tube's light on the surface, 0 beyond its range (TubeRangeWindow) or above a one-sided tube's face. Its intensity
// toward a direction: power * (faceShare * cos to the face normal (|cos| two-sided) + sideShare * |cos to the side axis|).
// Diffuse: the closed-form line integral, shared by the face and the sides. Specular, UE4's representative point: the
// point of the segment closest to the reflected ray, then the point of the tube's sphere of `radius` around it closest
// to the ray. Bsdf's specular (albedo 0) toward it, the tube's intensity toward it at its distance, times the energy
// normalisations of the widened lobe: alpha / alpha' for the length and (alpha / alpha'')^2 for the radius (alpha =
// roughness^2, alpha' = saturate(alpha + 0.5 length / distance), alpha'' = saturate(alpha + 0.5 radius / distance)).
float3 TubeLight(Surface s, float3 direction, RasterLine tube)
{
    float3 ld=tube.b-tube.a, l0=tube.a-s.position;
    float t=saturate(-dot(l0,ld)/max(dot(ld,ld),1e-12));
    float3 nearest=l0+ld*t;
    float range2=tube.range*tube.range, near2=dot(nearest,nearest);
    if(near2>=range2) return 0;
    float c=-dot(tube.normal,l0); // m.(p-a): the face's side of the surface point
    if(tube.twoSided!=0) c=abs(c); else if(c<=0) return 0;
    float window=saturate((range2-near2)/(range2*(1-0.75*0.75))); window*=window*(3-2*window);
    float3 outgoing=-direction, mid, power=tube.power*window;
    float3 result=0;
    float j=LineIntegral(s.position,s.normal,tube.a,tube.b,tube.radius,mid);
    if(j>0) result+=TubeDiffuse(s,outgoing,mid)*power*(j*(tube.faceShare*c+tube.sideShare*abs(dot(tube.side,l0))));
    float3 r=reflect(direction,s.normal);
    float rld=dot(r,ld); t=saturate((dot(r,l0)*rld-dot(l0,ld))/max(dot(ld,ld)-rld*rld,1e-8));
    float3 closest=l0+ld*t, toRay=dot(closest,r)*r-closest;
    float3 toLight=closest+toRay*saturate(tube.radius/max(length(toRay),1e-6));
    float distance2=max(dot(toLight,toLight),max(tube.radius*tube.radius,1e-8));
    float3 incoming=normalize(toLight);
    float nl=saturate(dot(s.normal,incoming)), face=-dot(tube.normal,incoming);
    float emitted=tube.faceShare*(tube.twoSided!=0 ? abs(face) : saturate(face))+tube.sideShare*abs(dot(tube.side,incoming));
    if(nl<=0 || emitted<=0) return result;
    Surface glossy=s; glossy.albedo=0;
    float alpha=s.roughness*s.roughness, inverse=rsqrt(max(dot(closest,closest),1e-8));
    float lengthNorm=alpha/max(saturate(alpha+0.5*length(ld)*inverse),1e-6);
    float sphereNorm=alpha/max(saturate(alpha+0.5*tube.radius*inverse),1e-6);
    return result+Bsdf(glossy,outgoing,incoming)*power*(emitted*nl/distance2)*lengthNorm*sphereNorm*sphereNorm;
}
#endif
