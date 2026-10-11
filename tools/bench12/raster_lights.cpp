#include "raster_lights.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace {
float Luminance(Vec3 rgb) { return Dot(rgb,{0.2126f,0.7152f,0.0722f}); }
struct Accumulator { Vec3 weighted, power; float weight=0; bool any=false; };
}

std::vector<RasterLight> BuildRasterLights(const std::vector<LampSource> &sources, const std::vector<Vec3> &centroids,
                                           const std::vector<LampGroup> &groups, std::vector<unsigned> *lightGroups)
{
    if(sources.size()!=centroids.size()) throw std::runtime_error("Lamp centroids do not match lamp sources");
    std::vector<Accumulator> sums(groups.size());
    for(size_t i=0;i<sources.size();++i) {
        const LampSource &s=sources[i];
        if(s.group>=groups.size()) throw std::runtime_error("Lamp source refers to a missing group");
        Accumulator &a=sums[s.group]; float w=s.area*Luminance(s.emission);
        a.any=true; a.weight+=w; a.weighted=a.weighted+centroids[i]*w; a.power=a.power+s.emission*s.area;
    }
    std::vector<RasterLight> lights;
    for(size_t g=0;g<groups.size();++g) {
        const Accumulator &a=sums[g];
        if(!a.any || !(a.weight>0)) continue;
        Vec3 f=LampFactor(groups[g]);
        RasterLight light;
        light.position=a.weighted*(1/a.weight);
        light.power={a.power.x*f.x,a.power.y*f.y,a.power.z*f.z};
        if(!std::isfinite(light.position.x+light.position.y+light.position.z+light.power.x+light.power.y+light.power.z))
            throw std::runtime_error("Invalid raster light");
        lights.push_back(light);
        if(lightGroups) lightGroups->push_back(unsigned(g));
    }
    return lights;
}
RasterLampLights BuildRasterLampTubes(const std::vector<LampSource> &sources, const std::vector<Vec3> &centroids,
                                      const std::vector<LampGroup> &groups, const std::vector<LampPart> &parts)
{
    if(sources.size()!=centroids.size()) throw std::runtime_error("Lamp centroids do not match lamp sources");
    RasterLampLights out; std::vector<char> inTube(sources.size(),0);
    for(const auto &part:parts) {
        if(!part.tube) continue;
        if(part.group>=groups.size()) throw std::runtime_error("Lamp part refers to a missing group");
        Vec3 sum{};
        for(unsigned s:part.sources) {
            if(s>=sources.size()) throw std::runtime_error("Lamp part refers to a missing source");
            inTube[s]=1; sum=sum+sources[s].emission*sources[s].area;
        }
        const Vec3 f=LampFactor(groups[part.group]);
        RasterLine line; line.a=part.a; line.b=part.b; line.radius=part.radius; line.power={sum.x*f.x,sum.y*f.y,sum.z*f.z};
        line.normal=part.normal; line.twoSided=part.twoSided ? 1.0f : 0.0f; line.range=TubeRange;
        line.faceShare=part.faceShare; line.sideShare=part.sideShare; line.side=Normalize(Cross(part.b-part.a,part.normal));
        if(!std::isfinite(Dot(line.a,line.a)+Dot(line.b,line.b)+line.radius+line.power.x+line.power.y+line.power.z))
            throw std::runtime_error("Invalid raster line light");
        if(line.power.x+line.power.y+line.power.z>0) { out.lines.push_back(line); out.lineGroups.push_back(part.group); }
    }
    std::vector<LampSource> compact; std::vector<Vec3> compactCentroids;
    for(size_t i=0;i<sources.size();++i) if(!inTube[i]) { compact.push_back(sources[i]); compactCentroids.push_back(centroids[i]); }
    out.points=BuildRasterLights(compact,compactCentroids,groups,&out.pointGroups);
    return out;
}
float TubeRangeWindow(float distance2, float range)
{
    const float range2=range*range, x=std::clamp((range2-distance2)/(range2*(1-0.75f*0.75f)),0.0f,1.0f);
    return x*x*(3-2*x);
}
float LineIrradiance(Vec3 p, Vec3 n, Vec3 a, Vec3 b, Vec3 m, bool twoSided, float radius)
{
    Vec3 l0=a-p, l1=b-p;
    float c=-Dot(m,l0); // m.(p-x), the same for every x of the segment (m is perpendicular to it)
    if(twoSided) c=std::abs(c); else if(c<=0) return 0;
    const float d0=Dot(n,l0), d1=Dot(n,l1), hold=std::max(radius,1e-4f), full=Length(b-a);
    if(d0<=0 && d1<=0) return 0;
    if(full<1e-6f) { const float r2=std::max(Dot(l0,l0),hold*hold); return c*d0/(r2*r2); }
    // The horizon: the part of the segment below the surface's plane is cut off; the integral over the rest is exact.
    if(d0<0) l0=l0+(l1-l0)*(d0/(d0-d1)); else if(d1<0) l1=l0+(l1-l0)*(d0/(d0-d1));
    // x = p + h + s u (h perpendicular to u): n.(x-p) / r^4 = (n.h + s n.u) / (h^2 + s^2)^2, integrated in closed form.
    const Vec3 u=(b-a)*(1/full);
    const float s0=Dot(l0,u), s1=Dot(l1,u); const Vec3 hv=l0-u*s0;
    const float h=std::max(Length(hv),hold), h2=h*h, nh=Dot(n,hv), nu=Dot(n,u);
    auto primitive=[&](float s) { const float q=h2+s*s; return nh*(s/(2*h2*q)+std::atan(s/h)/(2*h2*h))-nu/(2*q); };
    return c*(primitive(s1)-primitive(s0))/full;
}
float TubeIrradiance(Vec3 p, Vec3 n, const RasterLine &line)
{
    const bool twoSided=line.twoSided!=0;
    if(!twoSided && Dot(line.normal,p-line.a)<=0) return 0; // above the face's plane: nothing, the sides included
    return line.faceShare*LineIrradiance(p,n,line.a,line.b,line.normal,twoSided,line.radius)
        +line.sideShare*LineIrradiance(p,n,line.a,line.b,line.side,true,line.radius);
}
