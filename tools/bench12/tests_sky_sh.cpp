#include "tests_raster_support.h"
#include "sky_sh.h"
#include <vector>
using namespace raster_tests;
namespace {
HdrImage Panorama(unsigned w, unsigned h, Vec3 (*texel)(unsigned x, unsigned y, unsigned w, unsigned h))
{
    HdrImage image; image.width=w; image.height=h; image.pixels.resize(size_t(w)*h*4);
    for(unsigned y=0;y<h;++y) for(unsigned x=0;x<w;++x) {
        const Vec3 v=texel(x,y,w,h); float *p=&image.pixels[(size_t(y)*w+x)*4]; p[0]=v.x; p[1]=v.y; p[2]=v.z; p[3]=1;
    }
    return image;
}
bool Within(float value, float expected, float tolerance) { return std::abs(value-expected)<=tolerance*std::abs(expected); }
// A bright patch on the equator (|dy| <= h/8) at column `centre`/w, on a dim sky.
Vec3 EquatorPatch(unsigned x, unsigned y, unsigned w, unsigned h, unsigned centre)
{
    const bool on=std::abs(int(x)-int(centre))<=int(w/16) && std::abs(int(y)-int(h/2))<=int(h/8);
    return on ? Vec3{50,50,50} : Vec3{0.1f,0.1f,0.1f};
}
}
// --raster-sky-ambient: the SH9 irradiance of the HDRI (sky_sh.h) and the option's validity.
void SkyShChecks()
{
    const Vec3 normals[]={{0,1,0},{0,-1,0},{1,0,0},{0,0,-1},Normalize(Vec3{0.3f,0.5f,-0.8f})};
    constexpr float Pi=3.14159265f;
    {   // A constant panorama L: the irradiance is pi*L for every normal.
        const auto flat=Panorama(128,64,[](unsigned,unsigned,unsigned,unsigned) { return Vec3{2.0f,1.0f,0.5f}; });
        const auto sh=SkyIrradianceSH(flat,1);
        for(const Vec3 &n : normals) {
            const Vec3 e=EvalSH(sh,n);
            Require(Within(e.x,Pi*2.0f,0.02f) && Within(e.y,Pi*1.0f,0.02f) && Within(e.z,Pi*0.5f,0.02f),"constant sky: irradiance is pi*L");
        }
        const Vec3 scaled=EvalSH(SkyIrradianceSH(flat,0.5f),normals[0]);
        Require(Within(scaled.x,Pi,0.02f),"the scale multiplies the coefficients");
    }
    {   // Bright only above the horizon: up is brighter than down, and the sideways normal sits between them.
        const auto sky=Panorama(128,64,[](unsigned,unsigned y,unsigned,unsigned h) { return y<h/2 ? Vec3{3,3,3} : Vec3{0,0,0}; });
        const auto sh=SkyIrradianceSH(sky,1);
        const float up=EvalSH(sh,{0,1,0}).x, down=EvalSH(sh,{0,-1,0}).x, side=EvalSH(sh,{1,0,0}).x;
        Require(up>down*4 && up>side && side>down,"upper-hemisphere sky: up > side > down");
        // The exact irradiance of a lit upper hemisphere is pi*L straight up and pi*L/2 sideways (SH9 is close, not exact).
        Require(Within(up,Pi*3,0.1f) && Within(side,Pi*3/2,0.1f),"upper-hemisphere sky: values");
    }
    {   // The equirect convention of Sky(): the middle column is +x, the 3/4 column +z.
        const auto middle=Panorama(256,128,[](unsigned x,unsigned y,unsigned w,unsigned h) { return EquatorPatch(x,y,w,h,w/2); });
        const auto sh=SkyIrradianceSH(middle,1);
        const float px=EvalSH(sh,{1,0,0}).x, nx=EvalSH(sh,{-1,0,0}).x, pz=EvalSH(sh,{0,0,1}).x;
        Require(px>nx*3 && px>pz*2,"+x of the panorama is the middle column");
        const auto quarter=Panorama(256,128,[](unsigned x,unsigned y,unsigned w,unsigned h) { return EquatorPatch(x,y,w,h,3*w/4); });
        const auto q=SkyIrradianceSH(quarter,1);
        Require(EvalSH(q,{0,0,1}).x>EvalSH(q,{0,0,-1}).x*3,"3/4 of the panorama is +z");
    }
    Require(Rejected({"bench","--raster-sky-ambient"}),"--raster-sky-ambient without --render raster and --hdri accepted");
    Require(Rejected({"bench","--render","raster","--raster-sky-ambient"}),"--raster-sky-ambient without --hdri accepted");
    Require(Rejected({"bench","--hdri","sky.hdr","--raster-sky-ambient"}),"--raster-sky-ambient without --render raster accepted");
    Require(Parse({"bench","--render","raster","--hdri","sky.hdr","--raster-sky-ambient"}).rasterSkyAmbient,"--raster-sky-ambient parsed");
    Require(!Parse({"bench","--render","raster","--hdri","sky.hdr"}).rasterSkyAmbient,"--raster-sky-ambient is off by default");
    std::puts("[test] sky SH checks passed");
}
