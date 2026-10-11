// --sky-reflections: the prefilter and DFG math the shaders run (shaders/sky_reflection_math.hlsli through sky_reflections.h), the
// probes' sky visibility (probes.h) and the option.
#include "tests_raster_support.h"
#define PW_PREFILTER_SOURCE(direction,lod) TestSky(direction,lod)
namespace hlsl { struct float3; }
hlsl::float3 TestSky(hlsl::float3 direction, float lod);
#include "sky_reflections.h"
#include "probes.h"
#include <cstdio>
using namespace raster_tests;

namespace {
// The environment PrefilterGgx reads in the checks: a constant, or a white upper hemisphere over a black lower one.
int skyKind=0;
}
hlsl::float3 TestSky(hlsl::float3 direction, float)
{
    if(skyKind==0) return hlsl::float3(0.3f,0.5f,0.9f);
    return direction.y>0 ? hlsl::float3(1.0f) : hlsl::float3(0.0f);
}
namespace {
bool Close(float a, float b, float tolerance) { return std::abs(a-b)<=tolerance; }
// The specular reflectance of Bsdf()'s lobe (bsdf.hlsli: GGX, separable Smith, Schlick from v.h) for f0 = 0 and 1, by a midpoint
// rule over the hemisphere: the reference the importance-sampled DFG table must match.
hlsl::float2 ReferenceDfg(float nv, float roughness)
{
    using namespace hlsl;
    const float a2=std::pow(roughness,4.0f); const float3 v(std::sqrt(1-nv*nv),0,nv);
    const int thetaSteps=1024, phiSteps=256; double f0=0, f1=0;
    for(int t=0;t<thetaSteps;++t) {
        // Steps uniform in cos^2 theta crowd the samples near the normal, where a smooth lobe lives: d(cos^2) = 2 cos sin dtheta.
        const double c2=(t+0.5)/thetaSteps, nl=std::sqrt(c2), sl=std::sqrt(1-c2);
        for(int p=0;p<phiSteps;++p) {
            const double phi=SkyPi*(p+0.5)/phiSteps; // half the circle, mirrored (the lobe is symmetric in y)
            const float3 l(float(sl*std::cos(phi)),float(sl*std::sin(phi)),float(nl));
            const float3 h=normalize(v+l); const float vh=dot(v,h);
            const double spec=GgxDistribution(h.z,a2)*SmithG1(float(nl),a2)*SmithG1(nv,a2)/(4*nl*nv);
            const double fc=std::pow(1-vh,5.0);
            // n.l dl = n.l sin dtheta dphi = 0.5 d(cos^2) dphi; both halves of phi.
            const double weight=spec*0.5*(1.0/thetaSteps)*(SkyPi/phiSteps)*2;
            f0+=weight*fc; f1+=weight;
        }
    }
    return {float(f1-f0),float(f0)};
}
}

void ReflectionChecks()
{
    using namespace hlsl;
    {   // D3D's cube layout: the face centres are the axes, and each texel's direction selects its own face and coordinates back.
        const float3 centres[6]={float3(1,0,0),float3(-1,0,0),float3(0,1,0),float3(0,-1,0),float3(0,0,1),float3(0,0,-1)};
        for(uint f=0;f<6;++f) {
            const float3 d=CubeFaceDirection(f,0,0);
            Require(Close(dot(d,centres[f]),1,1e-6f),"cube face centres are +x, -x, +y, -y, +z, -z");
            for(float u:{-0.9f,-0.3f,0.5f}) for(float v:{-0.7f,0.2f,0.8f}) {
                const float3 r=CubeFaceDirection(f,u,v); // D3D's selection: the largest axis, then (sc / |ma|, tc / |ma|)
                const float ax=std::abs(r.x), ay=std::abs(r.y), az=std::abs(r.z);
                uint face; float sc, tc, ma;
                if(ax>=ay && ax>=az) { face=r.x>0 ? 0 : 1; ma=ax; sc=r.x>0 ? -r.z : r.z; tc=-r.y; }
                else if(ay>=az) { face=r.y>0 ? 2 : 3; ma=ay; sc=r.x; tc=r.y>0 ? r.z : -r.z; }
                else { face=r.z>0 ? 4 : 5; ma=az; sc=r.z>0 ? r.x : -r.x; tc=-r.y; }
                Require(face==f && Close(sc/ma,u,1e-5f) && Close(tc/ma,v,1e-5f),"cube texel directions follow D3D's face selection");
            }
        }
    }
    {   // The mip <-> roughness mapping is linear and covers 0..1.
        Require(SkyReflectionLod(0)==0 && Close(SkyReflectionLod(1),float(SkyReflectionMips-1),1e-6f) && Close(SkyMipRoughness(SkyReflectionMips-1),1,1e-6f),
                "prefiltered mips span roughness 0..1");
        for(uint m=0;m<SkyReflectionMips;++m) Require(Close(SkyReflectionLod(SkyMipRoughness(m)),float(m),1e-5f),"lod of a mip's roughness is the mip");
    }
    {   // A constant sky prefilters to itself at every roughness (the weights are normalised, no energy is lost or added).
        skyKind=0;
        for(uint m=1;m<SkyReflectionMips;++m) for(const float3 n:{float3(0,1,0),normalize(float3(1,-2,0.5f)),float3(0,0,-1)}) {
            const float3 c=PrefilterGgx(n,SkyMipRoughness(m),SkyPrefilterSamples);
            Require(Close(c.x,0.3f,1e-5f) && Close(c.y,0.5f,1e-5f) && Close(c.z,0.9f,1e-5f),"a constant sky prefilters to the same radiance at every roughness");
        }
        // A white upper hemisphere: 45 degrees up stays white when smooth and darkens as the lobe widens past the horizon; the
        // horizon itself is half at any roughness.
        skyKind=1; float last=2; const float3 tilted=normalize(float3(1,1,0));
        for(uint m=1;m<SkyReflectionMips;++m) {
            const float up=PrefilterGgx(tilted,SkyMipRoughness(m),SkyPrefilterSamples).x;
            Require(up<=last+1e-6f && up>0.6f,"prefiltered half sky 45 degrees up darkens with roughness"); last=up;
            const float side=PrefilterGgx(float3(1,0,0),SkyMipRoughness(m),SkyPrefilterSamples).x;
            Require(Close(side,0.5f,0.03f),"prefiltered horizon of a half sky is half");
        }
        Require(PrefilterGgx(tilted,SkyMipRoughness(1),SkyPrefilterSamples).x>0.995f && last<0.95f, // GGX's tail: 0.999 and 0.856
                "a smooth lobe 45 degrees up sees only the sky, a rough one also the ground");
    }
    {   // The DFG table (importance sampled, SkyDfgSamples = 4096) against the midpoint reference of Bsdf()'s own lobe.
        float worst=0;
        // From roughness 0.3: a smoother lobe is narrower than the reference grid near the mirror direction.
        for(float roughness:{0.3f,0.5f,0.7f,1.0f}) for(float nv:{0.1f,0.3f,0.6f,0.95f}) {
            const float2 table=IntegrateDfg(nv,roughness,SkyDfgSamples), reference=ReferenceDfg(nv,roughness);
            worst=std::max({worst,std::abs(table.x-reference.x),std::abs(table.y-reference.y)});
        }
        std::printf("[reflections] DFG table vs the reference: largest difference %.4f\n",double(worst));
        Require(worst<0.01f,"DFG table matches the reference within 0.01");
        // Physical bounds: a white f0 reflects at most everything; smooth and head-on it reflects almost all of it.
        for(float roughness:{0.04f,0.5f,1.0f}) for(float nv:{0.05f,0.5f,1.0f}) {
            const float2 ab=IntegrateDfg(nv,roughness,SkyDfgSamples);
            Require(ab.x>=0 && ab.y>=0 && ab.x+ab.y<=1.001f,"DFG: 0 <= A, B and A + B <= 1");
        }
        const float2 mirror=IntegrateDfg(1,0.04f,SkyDfgSamples);
        Require(mirror.x+mirror.y>0.99f && mirror.y<0.01f,"DFG: a smooth surface seen head-on reflects f0");
        const auto table=BuildDfgTable(8,256);
        Require(table.size()==8*8*2 && Close(table[(3*8+5)*2],IntegrateDfg(5.5f/8,3.5f/8,256).x,1e-6f),"DFG table: texel centres, n.v across, roughness down");
    }
    {   // The reflection's visibility: open stays open, the L1 ringing of a closed room goes, monotonic between.
        Require(ReflectionVisibility(1)==1 && ReflectionVisibility(SkyVisibilityFloor)==0 && ReflectionVisibility(0.1f)==0,
                "reflection visibility: 1 stays 1, the floor and below are 0");
        float last=-1; for(float v=0;v<=1;v+=0.05f) { const float r=ReflectionVisibility(v); Require(r>=last && r<=v+1e-6f,"reflection visibility rises, never above the probes'"); last=r; }
    }
    {   // Specular occlusion: none without occlusion, all with full occlusion, a rough lobe nearer the AO than a smooth one.
        Require(SpecularOcclusion(0.5f,1,0.5f)==1 && SpecularOcclusion(0.5f,0,0.5f)==0,"specular occlusion is 1 unoccluded and 0 occluded");
        Require(SpecularOcclusion(0.8f,0.5f,0.1f)>SpecularOcclusion(0.8f,0.5f,0.9f) && Close(SpecularOcclusion(0.8f,0.5f,1),0.5f,0.05f),
                "specular occlusion: smooth lobes keep more, rough ones follow the AO");
    }
    {   // The probes' sky visibility: an open sky above a ground plane, a closed room, a probe beside a wall; the lookup blends them.
        std::vector<Vec3> directions; std::vector<float> open, closed, wall;
        for(unsigned i=0;i<4096;++i) {
            const Vec3 d=FibonacciDirection(i,4096); directions.push_back(d);
            open.push_back(d.y>0 ? 1.0f : 0.0f); closed.push_back(0); wall.push_back(d.y>0 && d.x<0 ? 1.0f : 0.0f);
        }
        const auto sky=ProjectProbeSkyVisibility(directions,open), room=ProjectProbeSkyVisibility(directions,closed);
        const auto beside=ProjectProbeSkyVisibility(directions,wall);
        Require(Close(sky[0],0.5f,1e-3f) && EvalProbeSkyVisibility(sky,{0,1,0})==1 && EvalProbeSkyVisibility(sky,{0,-1,0})==0
                && Close(EvalProbeSkyVisibility(sky,{1,0,0}),0.5f,1e-2f),"sky visibility in the open: 1 up, half at the horizon, 0 down");
        Require(EvalProbeSkyVisibility(room,{0,1,0})==0 && EvalProbeSkyVisibility(room,{1,0,0})==0,"sky visibility in a closed room is 0");
        Require(EvalProbeSkyVisibility(beside,Normalize({-1,1,0}))>0.7f && EvalProbeSkyVisibility(beside,Normalize({1,1,0}))<0.3f,
                "sky visibility beside a wall: open away from it, closed into it");
        std::array<ProbeSkyVisibility,8> corners; for(unsigned k=0;k<8;++k) corners[k]=(k&1) ? room : sky;
        std::array<float,8> validity; validity.fill(1); std::array<float,8> side; side.fill(1);
        const auto half=InterpolateProbeSkyVisibility(ProbeWeights({0.5f,0.5f,0.5f},validity,side),corners);
        // Open alone: 0.5 + 0.75 up (1.25 before the clamp); halfway the coefficients halve: 0.625.
        Require(Close(EvalProbeSkyVisibility(half,{0,1,0}),0.625f,0.01f),"halfway between open and closed probes: half the coefficients");
        side.fill(0); for(unsigned k=0;k<8;++k) if(k&1) side[k]=1; // the open corners lie behind the surface
        Require(EvalProbeSkyVisibility(InterpolateProbeSkyVisibility(ProbeWeights({0.5f,0.5f,0.5f},validity,side),corners),{0,1,0})==0,
                "the lookup's side test drops the open corners behind a wall");
        validity.fill(0);
        Require(EvalProbeSkyVisibility(InterpolateProbeSkyVisibility(ProbeWeights({0.5f,0.5f,0.5f},validity,side),corners),{0,-1,0})==1,
                "no valid corner: open, as outside the grid");
    }
    {   // The option: auto with --hdri, explicit 0 or 1, rejects others; settings keep it; AO strength defaults to 1.
        Require(!Parse({"bench","--render","raster"}).SkyReflectionsOn() && Parse({"bench","--render","raster","--hdri","sky.hdr"}).SkyReflectionsOn(),
                "sky reflections default on with --hdri, off without");
        Require(!Parse({"bench","--render","raster","--hdri","sky.hdr","--sky-reflections","0"}).SkyReflectionsOn()
                && Parse({"bench","--render","raster","--sky-reflections","1"}).SkyReflectionsOn(),"--sky-reflections 0|1 overrides");
        Require(RejectedWith({"bench","--sky-reflections","2"},"--sky-reflections requires 0 or 1"),"--sky-reflections rejects 2");
        Settings saved; saved.options.skyReflections=0;
        Require(DecodeSettings(EncodeSettings(saved)).options.skyReflections==0,"settings keep sky reflections off");
        Require(DecodeSettings(EncodeSettings(Settings{})).options.skyReflections==-1,"settings keep the automatic default");
    }
    std::puts("[test] sky reflection checks passed");
}
