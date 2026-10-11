#include "tests_raster_support.h"
#include "probes.h"
#include <vector>
using namespace raster_tests;
namespace {
bool Within(float value, float expected, float tolerance) { return std::abs(value-expected)<=tolerance*std::abs(expected); }
bool Covers(const ProbeLayout &l, Vec3 lo, Vec3 hi)
{
    const float half=l.spacing*0.5f;
    const Vec3 first=l.origin-Vec3{half,half,half};
    const Vec3 last=l.Position(l.dims[0]-1,l.dims[1]-1,l.dims[2]-1)+Vec3{half,half,half};
    const float eps=1e-3f;
    return first.x<=lo.x+eps && first.y<=lo.y+eps && first.z<=lo.z+eps && last.x>=hi.x-eps && last.y>=hi.y-eps && last.z>=hi.z-eps;
}
ProbeSh Project(unsigned n, Vec3 (*radiance)(Vec3))
{
    std::vector<Vec3> directions, values;
    for(unsigned i=0;i<n;++i) { directions.push_back(FibonacciDirection(i,n)); values.push_back(radiance(directions.back())); }
    return ProjectProbeSh(directions,values);
}
float Sum(const std::array<float,8> &w) { float s=0; for(float v:w) s+=v; return s; }
}
// --probes: the grid layout, the L1 projection and evaluation, the lookup's corner weights, the options.
void ProbeChecks()
{
    constexpr float Pi=3.14159265f;
    {   // The layout covers the bounds at the requested spacing.
        const Vec3 lo{-5,0,-10}, hi{5,3,10};
        const auto l=ProbeGridLayout(lo,hi,1);
        Require(l.spacing==1 && l.dims[0]==10 && l.dims[1]==3 && l.dims[2]==20,"probe grid: 10 x 3 x 20 at 1 m");
        Require(Covers(l,lo,hi),"probe grid covers the bounds");
        const auto flat=ProbeGridLayout({0,2,0},{4,2,4},1);
        Require(flat.dims[1]==1 && Near(flat.origin.y,2) && Covers(flat,{0,2,0},{4,2,4}),"a flat scene gets one layer");
    }
    {   // A large scene: capped at 256 k probes, the spacing grown, still covering.
        const Vec3 lo{-500,0,-500}, hi{500,50,500};
        const auto l=ProbeGridLayout(lo,hi,1);
        Require(l.Count()<=ProbeMaxCount && l.Count()>ProbeMaxCount/2,"probe grid capped near 256 k");
        Require(l.spacing>1 && Covers(l,lo,hi),"the capped grid grows its spacing and covers the bounds");
        const auto thin=ProbeGridLayout({0,0,0},{5000,1,1},1);
        Require(thin.dims[0]<=ProbeMaxAxis && Covers(thin,{0,0,0},{5000,1,1}),"no axis above the 3D texture limit");
    }
    const Vec3 normals[]={{0,1,0},{0,-1,0},{1,0,0},{0,0,-1},Normalize(Vec3{0.3f,0.5f,-0.8f})};
    {   // A constant radiance L: pi * L for every normal, within 2 %.
        const auto sh=Project(256,[](Vec3) { return Vec3{2.0f,1.0f,0.5f}; });
        for(const Vec3 &n:normals) {
            const Vec3 e=EvalProbeSh(sh,n);
            Require(Within(e.x,Pi*2,0.02f) && Within(e.y,Pi,0.02f) && Within(e.z,Pi*0.5f,0.02f),"constant radiance: irradiance pi*L");
        }
    }
    {   // Radiance only from +y (the upper hemisphere, and a narrow cone): up > 0, down ~0 after max.
        const auto half=Project(256,[](Vec3 w) { return w.y>0 ? Vec3{3,3,3} : Vec3{}; });
        const float up=EvalProbeSh(half,{0,1,0}).x, down=std::max(EvalProbeSh(half,{0,-1,0}).x,0.0f);
        Require(Within(up,Pi*3,0.02f) && down<0.02f*up,"upper hemisphere: up pi*L, down ~0");
        const auto cone=Project(256,[](Vec3 w) { return w.y>0.9f ? Vec3{50,50,50} : Vec3{}; });
        Require(EvalProbeSh(cone,{0,1,0}).x>0 && std::max(EvalProbeSh(cone,{0,-1,0}).x,0.0f)==0,"cone from +y: down clamps to 0");
    }
    {   // Corner weights: one invalid corner renormalises the rest to 1; corners behind the surface weigh 0.
        const Vec3 f{0.3f,0.6f,0.5f};
        std::array<float,8> validity; validity.fill(1); std::array<float,8> side; side.fill(1);
        Require(std::abs(Sum(ProbeWeights(f,validity,side))-1)<1e-5f,"all corners valid: weights sum to 1");
        validity[3]=0;
        const auto w=ProbeWeights(f,validity,side);
        Require(w[3]==0 && std::abs(Sum(w)-1)<1e-5f,"one invalid corner: 0, the others renormalised to 1");
        validity.fill(1); for(unsigned k=0;k<8;++k) side[k]=(k&1) ? 1.0f : ProbeSide(-0.5f,1);
        const auto s=ProbeWeights(f,validity,side);
        Require(s[0]==0 && s[2]==0 && s[1]>0 && std::abs(Sum(s)-1)<1e-5f,"corners behind the surface weigh 0");
        side.fill(0);
        Require(std::abs(Sum(ProbeWeights(f,validity,side))-1)<1e-5f,"every valid corner behind: trilinear x validity");
        validity.fill(0);
        Require(Sum(ProbeWeights(f,validity,side))==0,"no valid corner: all 0 (the caller falls back)");
        Require(ProbeSide(0.2f,1)==1 && Near(ProbeSide(-0.05f,1),0.5f) && ProbeSide(-0.2f,1)==0,"side band of 0.1 spacing");
    }
    {   // Visibility, spacing 2: a roof 0.66 below a probe hides points further down; the open directions stay visible.
        const float packed[3]={PackProbeFree(1,1,2),PackProbeFree(2,0.66f,2),PackProbeFree(2,2,2)};
        Require(PackProbeFree(2,0.5f,2)==31*32+8 && PackProbeFree(5,5,2)==1023,"free distances pack into 0..1023 (exact in FP16)");
        Require(ProbeVisible({0.4f,-0.2f,-1.5f},packed,2)==1,"a point in front of the roof is visible");
        Require(ProbeVisible({0.4f,-0.9f,0.3f},packed,2)==0,"a point beyond the roof is hidden");
        Require(ProbeVisible({-1.2f,0.5f,0},packed,2)==0 && ProbeVisible({-0.9f,1.9f,0},packed,2)==1,"each axis in its direction");
    }
    Require(Rejected({"bench","--probes"}),"--probes without --render raster accepted");
    Require(Rejected({"bench","--render","pathtrace","--probes"}),"--probes with --render pathtrace accepted");
    Require(Rejected({"bench","--render","raster","--probe-spacing","2"}),"--probe-spacing without --probes accepted");
    Require(Rejected({"bench","--render","raster","--probe-rays","128"}),"--probe-rays without --probes accepted");
    Require(Rejected({"bench","--render","raster","--probes","--probe-spacing","0"}),"--probe-spacing 0 accepted");
    Require(Rejected({"bench","--render","raster","--probes","--probe-rays","8"}),"--probe-rays 8 accepted");
    const auto parsed=Parse({"bench","--render","raster","--probes","--probe-spacing","0.5","--probe-rays","512"});
    Require(parsed.probes && parsed.probeSpacing==0.5f && parsed.probeRays==512,"--probes options parsed");
    const auto defaults=Parse({"bench","--render","raster"});
    Require(!defaults.probes && defaults.probeSpacing==1 && defaults.probeRays==256,"--probes off by default, 1 m, 256 rays");
    std::puts("[test] probe checks passed");
}
