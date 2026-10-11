#pragma once
// --probes: irradiance light probes for --render raster. A regular grid over the scene's bounds, traced once at start with
// the path tracer's own functions (probes.cpp, shaders/probes_trace.hlsl), stored as L1 spherical harmonics in four 3D
// textures and read by the scene pass's ambient term (shaders/raster_probes.hlsli). The CPU helpers here are the layout
// and the math those shaders mirror; tests_probes.cpp checks them.
#include "device.h"
#include "math.h"
#include <array>
#include <cmath>
#include <vector>

class Scene;
class Accel;
struct Options;

constexpr unsigned ProbeMaxCount=262144;  // 256 k probes: the spacing grows until the grid fits
constexpr unsigned ProbeMaxAxis=2048;     // a 3D texture's largest dimension
constexpr float ProbeInvalidBackFaces=0.3f; // more back-face hits than this fraction: the probe sits inside geometry
constexpr float ProbeSideBand=0.1f;       // a corner probe this far (x spacing) behind the shaded surface weighs 0
constexpr float ProbeNormalBias=0.2f;     // the lookup point moves this far (x spacing) along the surface normal

// Probe (i,j,k) sits at origin + (i,j,k) * spacing. Each probe stands for a cell of one spacing around it, so the grid
// covers origin - spacing/2 .. origin + (dims - 1/2) * spacing; the lookup clamps inside that box and falls back outside.
struct ProbeLayout {
    Vec3 origin; float spacing=1; unsigned dims[3]{1,1,1};
    unsigned Count() const { return dims[0]*dims[1]*dims[2]; }
    Vec3 Position(unsigned i, unsigned j, unsigned k) const { return origin+Vec3{float(i),float(j),float(k)}*spacing; }
};
// The grid over [boundsMin, boundsMax], centred on it: ceil(extent / spacing) probes per axis (at least 1). The spacing
// grows by 2 % steps until the grid holds at most maxCount probes and no axis exceeds ProbeMaxAxis.
inline ProbeLayout ProbeGridLayout(Vec3 boundsMin, Vec3 boundsMax, float spacing, unsigned maxCount=ProbeMaxCount)
{
    const float extent[3]={std::max(0.0f,boundsMax.x-boundsMin.x),std::max(0.0f,boundsMax.y-boundsMin.y),std::max(0.0f,boundsMax.z-boundsMin.z)};
    ProbeLayout l; l.spacing=spacing;
    for(;;) {
        double count=1; bool fits=true;
        for(int a=0;a<3;++a) {
            const double n=std::max(1.0,std::ceil(double(extent[a])/l.spacing));
            fits&=n<=ProbeMaxAxis; count*=n; l.dims[a]=unsigned(std::min(n,double(ProbeMaxAxis)));
        }
        if(fits && count<=maxCount) break;
        l.spacing*=1.02f;
    }
    const Vec3 centre=(boundsMin+boundsMax)*0.5f;
    l.origin=centre-Vec3{float(l.dims[0]-1),float(l.dims[1]-1),float(l.dims[2]-1)}*(0.5f*l.spacing);
    return l;
}
// Ray i of n: the spherical Fibonacci set (uniform on the sphere, the same for every probe). probes_trace.hlsl mirrors it.
inline Vec3 FibonacciDirection(unsigned i, unsigned n)
{
    const float y=1-2*(float(i)+0.5f)/float(n), r=std::sqrt(std::max(0.0f,1-y*y)), phi=float(i)*2.39996323f;
    return {std::cos(phi)*r,y,std::sin(phi)*r};
}
// Irradiance in L1 SH, already convolved with the cosine lobe: E(n) = c[0] + c[1] n.x + c[2] n.y + c[3] n.z.
// From radiance L sampled in n uniform directions w: c[0] = pi * mean(L), c[1..3] = 2 pi * mean(L * w) (band 0 times pi,
// band 1 times 2 pi / 3, with Y00^2 = 1 / 4 pi and Y1m^2 = 3 / 4 pi folded in). A constant L gives pi * L for every n.
struct ProbeSh { Vec3 c[4]; };
inline ProbeSh ProjectProbeSh(const std::vector<Vec3> &directions, const std::vector<Vec3> &radiance)
{
    ProbeSh sh{}; if(directions.empty() || directions.size()!=radiance.size()) return sh;
    for(size_t i=0;i<directions.size();++i) {
        const Vec3 L=radiance[i], w=directions[i];
        sh.c[0]=sh.c[0]+L; sh.c[1]=sh.c[1]+L*w.x; sh.c[2]=sh.c[2]+L*w.y; sh.c[3]=sh.c[3]+L*w.z;
    }
    constexpr float Pi=3.14159265f; const float inverse=1.0f/float(directions.size());
    sh.c[0]=sh.c[0]*(Pi*inverse); for(int k=1;k<4;++k) sh.c[k]=sh.c[k]*(2*Pi*inverse);
    return sh;
}
inline Vec3 EvalProbeSh(const ProbeSh &sh, Vec3 n) { return sh.c[0]+sh.c[1]*n.x+sh.c[2]*n.y+sh.c[3]*n.z; }
// The side test of one corner: distance = dot(probe - surface point, surface normal). 1 in front, 0 from ProbeSideBand
// spacings behind the surface, linear between.
inline float ProbeSide(float distance, float spacing) { return std::clamp(1+distance/(ProbeSideBand*spacing),0.0f,1.0f); }
// Probe visibility. Each probe traces the six axis directions up to one spacing; the free distance along +a and -a
// (fractions of the spacing, quantised to 0..31) share the alpha of coefficient a+1: alpha = 32 q(+a) + q(-a), exact in
// FP16. A lookup point whose offset from a corner probe exceeds the free distance along an axis, in that axis's direction,
// lies behind geometry from that probe (a thin roof, a wall), and the corner is dropped like one behind the surface.
constexpr float ProbeFreeLevels=31, ProbeFreeTolerance=0.05f; // tolerance in spacings
inline float PackProbeFree(float positive, float negative, float spacing)
{
    auto q=[&](float d) { return std::round(std::clamp(d/spacing,0.0f,1.0f)*ProbeFreeLevels); };
    return q(positive)*32+q(negative);
}
// 1 when `offset` (lookup point minus probe, scene units) stays within the probe's free distances, else 0.
inline float ProbeVisible(Vec3 offset, const float packed[3], float spacing)
{
    const float o[3]={offset.x,offset.y,offset.z};
    for(int a=0;a<3;++a) {
        const float high=std::floor(packed[a]/32), low=packed[a]-high*32;
        const float free=(o[a]>=0 ? high : low)/ProbeFreeLevels*spacing;
        if(std::abs(o[a])>free+ProbeFreeTolerance*spacing) return 0;
    }
    return 1;
}
// The eight corner weights of a lookup (corner k: offset (k&1, k>>1&1, k>>2)), f the position inside the cell:
// trilinear x validity x side, renormalised to 1; `side` is ProbeSide times ProbeVisible. When every valid corner lies
// behind the surface or behind geometry, that test is dropped (trilinear x validity); with no valid corner all weights are 0
// and the caller falls back.
// shaders/raster_probes.hlsli computes the same.
inline std::array<float,8> ProbeWeights(Vec3 f, const std::array<float,8> &validity, const std::array<float,8> &side)
{
    std::array<float,8> sided{}, valid{}; float sidedSum=0, validSum=0;
    for(unsigned k=0;k<8;++k) {
        const float w=((k&1) ? f.x : 1-f.x)*((k&2) ? f.y : 1-f.y)*((k&4) ? f.z : 1-f.z)*validity[k];
        valid[k]=w; sided[k]=w*side[k]; validSum+=w; sidedSum+=sided[k];
    }
    auto &chosen=sidedSum>1e-6f ? sided : valid; const float sum=sidedSum>1e-6f ? sidedSum : validSum;
    std::array<float,8> out{};
    if(sum>1e-6f) for(unsigned k=0;k<8;++k) out[k]=chosen[k]/sum;
    return out;
}

// The sky visibility of a probe (--sky-reflections): V(d), the chance that a ray along d escapes to the sky, in L1 from the escape
// indicator e of n uniform directions w: v[0] = mean(e), v[1..3] = 3 mean(e w), so V(d) = saturate(v[0] + v[1..3] . d). Open
// sky above a ground plane: 1 up, 0.5 at the horizon, 0 down; inside a closed room 0 everywhere. The lookup interpolates the
// four values with ProbeWeights, as the irradiance. shaders/probes_trace.hlsl and raster_reflections.hlsli compute the same.
using ProbeSkyVisibility=std::array<float,4>;
inline ProbeSkyVisibility ProjectProbeSkyVisibility(const std::vector<Vec3> &directions, const std::vector<float> &escaped)
{
    ProbeSkyVisibility v{}; if(directions.empty() || directions.size()!=escaped.size()) return v;
    for(size_t i=0;i<directions.size();++i) {
        const float e=escaped[i]; v[0]+=e; v[1]+=e*directions[i].x; v[2]+=e*directions[i].y; v[3]+=e*directions[i].z;
    }
    const float inverse=1.0f/float(directions.size());
    v[0]*=inverse; for(int k=1;k<4;++k) v[k]*=3*inverse;
    return v;
}
inline float EvalProbeSkyVisibility(const ProbeSkyVisibility &v, Vec3 d) { return std::clamp(v[0]+v[1]*d.x+v[2]*d.y+v[3]*d.z,0.0f,1.0f); }
// The lookup's visibility: the corners' coefficients weighted by ProbeWeights (all 0: open, as outside the grid).
inline ProbeSkyVisibility InterpolateProbeSkyVisibility(const std::array<float,8> &weights, const std::array<ProbeSkyVisibility,8> &corners)
{
    ProbeSkyVisibility v{}; float sum=0;
    for(unsigned k=0;k<8;++k) { for(int c=0;c<4;++c) v[size_t(c)]+=weights[k]*corners[k][size_t(c)]; sum+=weights[k]; }
    if(sum<=1e-6f) return {1,0,0,0};
    return v;
}

// The traced grid on the GPU (ProbeLayout's placement): sh[k] is a dims[0] x dims[1] x dims[2] RGBA16F texture holding
// coefficient k of every probe, rgb the irradiance term; alpha of sh[0] the validity (1, or 0 inside geometry), of sh[1..3]
// the packed free distances along x, y, z (PackProbeFree); `sky` the same size, the sky visibility (ProbeSkyVisibility).
// PIXEL_SHADER_RESOURCE once built; Raster::UseProbes binds them to the scene pass (shaders/raster_probes.hlsli).
struct ProbeGrid {
    Vec3 origin; float spacing=1; unsigned dims[3]{};
    ComPtr<ID3D12Resource> sh[4], sky;
};
// Once after the scene and its acceleration structures exist, before the first frame: traces every probe on the static
// geometry (the dynamic triangles only receive; the sky visibility also sees --wind's foliage), writes the textures, prints the
// cost and, with --probe-dump, the grid.
ProbeGrid BuildProbes(Device &device, const Scene &scene, const Accel &accel, const Options &options);
