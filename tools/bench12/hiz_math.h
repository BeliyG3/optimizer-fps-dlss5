#pragma once
// The Hi-Z occlusion test of shaders/raster_occlusion.hlsl in C++, line for line, for the unit tests (tests_culling.cpp). The GPU
// pads level 0 to powers of two (raster_occlusion.cpp, D3D12 mip sizes round down) where this rounds each level's size up; the
// padding texels hold clamped copies of edge texels, which can only make a texel farther (cull less), never nearer.
// The Hi-Z holds, per texel of level L, the farthest depth of the 2^(L+1) x 2^(L+1) pixels it covers (level 0 is half the
// render size, rounded up; each level halves again, rounded up; reads past the last texel clamp to it, which stays inside
// the texel's own pixels). A box is occluded when its nearest depth is farther than the farthest occluder depth of every
// pixel its projection may touch: then every fragment it could write fails the depth test.
#include "math.h"
#include <vector>

struct HiZ {
    unsigned width=0, height=0; // the depth buffer's
    std::vector<unsigned> w, h; // per level
    std::vector<std::vector<float>> levels;
    float At(unsigned level, unsigned x, unsigned y) const { return levels[level][size_t(y)*w[level]+x]; }
};
inline float Farther(float a, float b, bool reverse) { return reverse ? std::min(a,b) : std::max(a,b); }
inline HiZ BuildHiZ(const std::vector<float> &depth, unsigned width, unsigned height, bool reverse)
{
    HiZ z; z.width=width; z.height=height;
    unsigned sw=width, sh=height; const std::vector<float> *source=&depth;
    while(true) {
        const unsigned dw=(sw+1)/2, dh=(sh+1)/2;
        std::vector<float> level(size_t(dw)*dh);
        for(unsigned y=0;y<dh;++y) for(unsigned x=0;x<dw;++x) {
            const unsigned x0=std::min(2*x,sw-1), x1=std::min(2*x+1,sw-1), y0=std::min(2*y,sh-1), y1=std::min(2*y+1,sh-1);
            const auto &s=*source;
            level[size_t(y)*dw+x]=Farther(Farther(s[size_t(y0)*sw+x0],s[size_t(y0)*sw+x1],reverse),
                                         Farther(s[size_t(y1)*sw+x0],s[size_t(y1)*sw+x1],reverse),reverse);
        }
        z.w.push_back(dw); z.h.push_back(dh); z.levels.push_back(std::move(level)); source=&z.levels.back();
        if(dw==1 && dh==1) break;
        sw=dw; sh=dh;
    }
    return z;
}
// The cull shader's test. `viewProjection`: the unjittered camera (clip w = view depth, near plane at w = 0.1); the projected
// rectangle grows by `marginPixels` for the jitter. Conservative: a corner in front of the near plane, or a rectangle off the
// screen, is visible.
inline bool HiZOccluded(const HiZ &z, const Mat4 &viewProjection, Vec3 lo, Vec3 hi, bool reverse, float marginPixels=1)
{
    float minX=1e30f, minY=1e30f, maxX=-1e30f, maxY=-1e30f, nearest=reverse ? 0.0f : 1.0f;
    for(unsigned k=0;k<8;++k) {
        const Vec4 p{k&1 ? hi.x : lo.x,k&2 ? hi.y : lo.y,k&4 ? hi.z : lo.z,1};
        const Vec4 c=Mul(viewProjection,p);
        if(c.w<0.1f) return false;
        const float x=(c.x/c.w*0.5f+0.5f)*float(z.width), y=(0.5f-c.y/c.w*0.5f)*float(z.height), depth=c.z/c.w;
        minX=std::min(minX,x); maxX=std::max(maxX,x); minY=std::min(minY,y); maxY=std::max(maxY,y);
        nearest=reverse ? std::max(nearest,depth) : std::min(nearest,depth);
    }
    const float x0=std::max(std::floor(minX-marginPixels),0.0f), x1=std::min(std::floor(maxX+marginPixels),float(z.width)-1);
    const float y0=std::max(std::floor(minY-marginPixels),0.0f), y1=std::min(std::floor(maxY+marginPixels),float(z.height)-1);
    if(x0>x1 || y0>y1) return false;
    unsigned tx0=unsigned(x0)>>1, tx1=unsigned(x1)>>1, ty0=unsigned(y0)>>1, ty1=unsigned(y1)>>1, level=0;
    while((tx1-tx0>1 || ty1-ty0>1) && level+1<z.levels.size()) { tx0>>=1; tx1>>=1; ty0>>=1; ty1>>=1; ++level; }
    float farthest=reverse ? 1.0f : 0.0f;
    for(unsigned y=ty0;y<=ty1;++y) for(unsigned x=tx0;x<=tx1;++x) farthest=Farther(farthest,z.At(level,x,y),reverse);
    return reverse ? nearest<farthest : nearest>farthest;
}
