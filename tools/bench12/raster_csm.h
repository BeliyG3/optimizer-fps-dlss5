#pragma once
#include "math.h"
#include <array>

// Cascaded sun shadow maps for the raster path: the view frustum is cut into four slices and each slice gets its own
// orthographic map along the sun's travel (the layout of SunShadow in raster_shadow.h: clip x,y in [-1,1], depth 0..1).
struct Cascade {
    Mat4 viewProjection;
    float texelWorld=0;  // one shadow-map texel, in scene units
    float depthRange=0;  // scene units covered by depth 0..1
    float splitFar=0;    // view distance of the slice's far end
};
struct CascadeSet { std::array<Cascade,4> cascades; float blendFraction=0.1f; };

// Far end of each slice, the practical scheme: a mix of the logarithmic and the linear split.
inline std::array<float,4> CascadeSplits(float nearZ, float farZ, float lambda=0.75f)
{
    std::array<float,4> s{};
    for(int i=1;i<=4;++i) {
        const float t=float(i)/4;
        s[i-1]=lambda*nearZ*std::pow(farZ/nearZ,t)+(1-lambda)*(nearZ+(farZ-nearZ)*t);
    }
    s[3]=farZ;
    return s;
}

// eye/forward/up: the unjittered camera; tanHalfY and aspect: its projection; sceneLo/Hi: the scene bounds (the light
// depth covers them, so casters outside a slice still cast). stable: the slice's bounding sphere sets the extent, so it
// does not change with the view direction, and each origin snaps to its texel grid (the map does not swim when the
// camera moves). The sphere gets a one-texel border so the snap cannot push a slice corner out of the map.
inline CascadeSet FitCascades(Vec3 eye, Vec3 forward, Vec3 up, float tanHalfY, float aspect, float nearZ, float farZ,
                              Vec3 sunTravel, Vec3 sceneLo, Vec3 sceneHi, unsigned resolution, bool stable)
{
    const Vec3 sun=Normalize(sunTravel), helper=std::abs(sun.y)<0.99f ? Vec3{0,1,0} : Vec3{1,0,0};
    const Vec3 lr=Normalize(Cross(helper,sun)), lu=Cross(sun,lr), axes[]={lr,lu,sun};
    const Vec3 fw=Normalize(forward);
    Vec3 right=Cross(up,fw);
    if(Dot(right,right)<1e-8f) right=Cross({1,0,0},fw);
    if(Dot(right,right)<1e-8f) right=Cross({0,0,1},fw);
    right=Normalize(right);
    const Vec3 camUp=Cross(fw,right);
    float sceneLow=1e30f, sceneHigh=-1e30f;
    for(int corner=0;corner<8;++corner) {
        const Vec3 p{corner&1 ? sceneHi.x : sceneLo.x,corner&2 ? sceneHi.y : sceneLo.y,corner&4 ? sceneHi.z : sceneLo.z};
        const float d=Dot(p,sun); sceneLow=std::min(sceneLow,d); sceneHigh=std::max(sceneHigh,d);
    }
    const auto splits=CascadeSplits(nearZ,farZ);
    CascadeSet set;
    for(int c=0;c<4;++c) {
        // Slice c+1 starts at the start of slice c's blend band, where the shader begins to blend toward it. The band
        // is blendFraction of the slice measured from view depth 0 (CsmSunVisibility in shaders/raster_csm.hlsli).
        const float band=c==0 ? 0 : (splits[c-1]-(c==1 ? 0 : splits[c-2]))*set.blendFraction;
        const float n=c==0 ? nearZ : splits[c-1]-band, f=splits[c];
        Vec3 corners[8], centre{};
        float low[3]={1e30f,1e30f,sceneLow}, high[3]={-1e30f,-1e30f,sceneHigh};
        for(int k=0;k<8;++k) {
            const float d=(k&4) ? f : n, x=((k&1) ? 1 : -1)*d*tanHalfY*aspect, y=((k&2) ? 1 : -1)*d*tanHalfY;
            corners[k]=eye+right*x+camUp*y+fw*d; centre=centre+corners[k]*0.125f;
            for(int a=0;a<3;++a) { const float v=Dot(corners[k],axes[a]); low[a]=std::min(low[a],v); high[a]=std::max(high[a],v); }
        }
        Cascade &cascade=set.cascades[c];
        if(stable) {
            float radius=0;
            for(const Vec3 &p : corners) { const Vec3 d=p-centre; radius=std::max(radius,std::sqrt(Dot(d,d))); }
            radius=std::ceil(radius*16)/16;
            const float texel=2*radius/float(resolution-2), half=texel*float(resolution)/2;
            for(int a=0;a<2;++a) { const float o=std::floor(Dot(centre,axes[a])/texel)*texel; low[a]=o-half; high[a]=o+half; }
            cascade.texelWorld=texel;
        } else cascade.texelWorld=std::max(high[0]-low[0],high[1]-low[1])/float(resolution);
        const float margin=std::max(0.01f*(high[2]-low[2]),0.01f); low[2]-=margin; high[2]+=margin;
        cascade.viewProjection=Identity();
        for(int a=0;a<3;++a) {
            const float scale=a<2 ? 2/(high[a]-low[a]) : 1/(high[a]-low[a]);
            const float offset=a<2 ? -(high[a]+low[a])/(high[a]-low[a]) : -low[a]/(high[a]-low[a]);
            cascade.viewProjection.m[a][0]=axes[a].x*scale; cascade.viewProjection.m[a][1]=axes[a].y*scale;
            cascade.viewProjection.m[a][2]=axes[a].z*scale; cascade.viewProjection.m[a][3]=offset;
        }
        cascade.depthRange=high[2]-low[2]; cascade.splitFar=f;
    }
    return set;
}
