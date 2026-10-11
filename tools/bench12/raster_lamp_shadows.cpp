// --lamp-shadows, CPU side (raster_lamp_shadows.h): the groups' spot frusta and the visibility weights.
#include "raster_lamp_shadows.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace {
constexpr float Pi=3.14159265f;
float Luminance(Vec3 rgb) { return Dot(rgb,{0.2126f,0.7152f,0.0722f}); }
struct Bounds { Vec3 lo{1e30f,1e30f,1e30f}, hi{-1e30f,-1e30f,-1e30f}, weighted; float weight=0; bool any=false; };
// A perspective view down LampShadowAxis from the apex (right = -z, up = +x: FitSunShadow's basis for a vertical travel),
// standard depth from nearPlane (0) to farPlane (1).
Mat4 SpotViewProjection(Vec3 apex, float fov, float nearPlane, float farPlane)
{
    const Vec3 f=LampShadowAxis, r{0,0,-1}, u=Cross(f,r), axes[]={r,u,f};
    Mat4 view=Identity();
    for(int i=0;i<3;++i) { view.m[i][0]=axes[i].x; view.m[i][1]=axes[i].y; view.m[i][2]=axes[i].z; view.m[i][3]=-Dot(axes[i],apex); }
    Mat4 projection; const float focal=1/std::tan(0.5f*fov);
    projection.m[0][0]=focal; projection.m[1][1]=focal;
    projection.m[2][2]=farPlane/(farPlane-nearPlane); projection.m[2][3]=-nearPlane*farPlane/(farPlane-nearPlane); projection.m[3][2]=1;
    return Mul(projection,view);
}
}

std::vector<LampShadowFrustum> FitLampShadows(const std::vector<LampSource> &sources, const std::vector<LampTriangle> &triangles,
                                              size_t groupCount, float floorY)
{
    if(sources.size()!=triangles.size()) throw std::runtime_error("Lamp triangles do not match lamp sources");
    std::vector<Bounds> groups(groupCount);
    for(size_t i=0;i<sources.size();++i) {
        const LampSource &s=sources[i];
        if(s.group>=groupCount) throw std::runtime_error("Lamp source refers to a missing group");
        Bounds &b=groups[s.group]; const float w=s.area*Luminance(s.emission);
        const Vec3 centroid=(triangles[i][0]+triangles[i][1]+triangles[i][2])*(1.0f/3);
        b.any=true; b.weight+=w; b.weighted=b.weighted+centroid*w;
        for(const Vec3 &c:triangles[i]) {
            b.lo={std::min(b.lo.x,c.x),std::min(b.lo.y,c.y),std::min(b.lo.z,c.z)};
            b.hi={std::max(b.hi.x,c.x),std::max(b.hi.y,c.y),std::max(b.hi.z,c.z)};
        }
    }
    std::vector<LampShadowFrustum> frusta;
    for(size_t g=0;g<groupCount && frusta.size()<MaxLampShadows;++g) {
        const Bounds &b=groups[g];
        if(!b.any || !(b.weight>0)) continue;
        LampShadowFrustum f; f.group=unsigned(g); f.centre=b.weighted*(1/b.weight);
        const float dx=b.hi.x-b.lo.x, dz=b.hi.z-b.lo.z, e=0.5f*std::sqrt(dx*dx+dz*dz);
        const float h=std::max(f.centre.y-floorY,0.5f);
        f.litRadius=e+std::sqrt(3.0f)*h;
        f.apex={0.5f*(b.lo.x+b.hi.x),std::max(f.centre.y+e,b.hi.y+0.1f),0.5f*(b.lo.z+b.hi.z)};
        const float height=f.apex.y-floorY;
        f.fov=std::clamp(2*std::atan(f.litRadius/height),0.5f*Pi,2*Pi/3);
        f.nearPlane=std::max(f.apex.y-(b.lo.y-0.05f),0.05f);
        f.farPlane=std::max(height+0.05f,f.nearPlane+0.1f);
        f.texelScale=2*std::tan(0.5f*f.fov)/float(LampShadowResolution);
        f.viewProjection=SpotViewProjection(f.apex,f.fov,f.nearPlane,f.farPlane);
        frusta.push_back(f);
    }
    return frusta;
}
std::vector<unsigned> LampShadowSlices(const std::vector<LampShadowFrustum> &frusta, size_t groupCount)
{
    std::vector<unsigned> slices(groupCount,NoLampShadow);
    for(size_t i=0;i<frusta.size();++i) if(frusta[i].group<groupCount) slices[frusta[i].group]=unsigned(i);
    return slices;
}
float LampShadowWeight(const LampShadowFrustum &frustum, float powerLuminance, Vec3 p)
{
    const Vec3 d=frustum.centre-p;
    return powerLuminance*0.25f/std::max(Dot(d,d),0.25f*0.25f);
}
float LampVisibility(const std::vector<LampShadowFrustum> &frusta, const std::vector<float> &powerLuminance,
                     const std::vector<float> &visibility, Vec3 p)
{
    float sum=0, weight=0;
    for(size_t i=0;i<frusta.size() && i<powerLuminance.size() && i<visibility.size();++i) {
        const float w=LampShadowWeight(frusta[i],powerLuminance[i],p);
        if(w>0) { sum+=w*visibility[i]; weight+=w; }
    }
    return weight>0 ? sum/weight : 1;
}
