#pragma once
#include "math.h"

// The raster sun shadow map: an orthographic projection along the light's travel, fitted to the scene bounds.
// Clip x,y cover the bounds in [-1,1]; depth is 0 at the side nearest the sun and 1 at the far side.
struct SunShadow {
    Mat4 viewProjection;
    float texelWorld=0;  // one shadow-map texel, in scene units
    float depthRange=0;  // scene units covered by depth 0..1
};
inline SunShadow FitSunShadow(Vec3 sunTravel, Vec3 lo, Vec3 hi, unsigned resolution)
{
    const Vec3 f=Normalize(sunTravel), helper=std::abs(f.y)<0.99f ? Vec3{0,1,0} : Vec3{1,0,0};
    const Vec3 r=Normalize(Cross(helper,f)), u=Cross(f,r), axes[]={r,u,f};
    float low[3]={1e30f,1e30f,1e30f}, high[3]={-1e30f,-1e30f,-1e30f};
    for(int corner=0;corner<8;++corner) {
        const Vec3 p{corner&1 ? hi.x : lo.x,corner&2 ? hi.y : lo.y,corner&4 ? hi.z : lo.z};
        for(int a=0;a<3;++a) { const float d=Dot(p,axes[a]); low[a]=std::min(low[a],d); high[a]=std::max(high[a],d); }
    }
    SunShadow s; s.viewProjection=Identity();
    for(int a=0;a<3;++a) {
        // A 1% margin (at least 1 cm) keeps the bounding faces off the clip planes.
        const float margin=std::max(0.01f*(high[a]-low[a]),0.01f); low[a]-=margin; high[a]+=margin;
        const float scale=a<2 ? 2/(high[a]-low[a]) : 1/(high[a]-low[a]);
        const float offset=a<2 ? -(high[a]+low[a])/(high[a]-low[a]) : -low[a]/(high[a]-low[a]);
        s.viewProjection.m[a][0]=axes[a].x*scale; s.viewProjection.m[a][1]=axes[a].y*scale;
        s.viewProjection.m[a][2]=axes[a].z*scale; s.viewProjection.m[a][3]=offset;
    }
    s.texelWorld=std::max(high[0]-low[0],high[1]-low[1])/float(resolution);
    s.depthRange=high[2]-low[2];
    return s;
}
