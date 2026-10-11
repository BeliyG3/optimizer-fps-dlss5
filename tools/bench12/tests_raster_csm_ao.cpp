#include "tests_raster_support.h"
#include "ao_math.h"
#include "camera.h"
#include "raster_csm.h"
#include "settings.h"
#include "shaders/raster_depth.hlsli"
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>
using namespace raster_tests;
namespace {
// Cascaded sun shadows (raster_csm.h) and the --csm/--ao options.
void CsmChecks()
{
    const Vec3 lo{-30,-1,-30}, hi{30,10,30}, sun=Normalize(Vec3{0.45f,-0.77f,0.45f});
    {   // Splits: increasing, last = far, lambda 0.75.
        const auto s=CascadeSplits(0.1f,100.f);
        Require(s[0]>0.1f && s[0]<s[1] && s[1]<s[2] && s[2]<s[3] && std::abs(s[3]-100.f)<1e-3f,"CascadeSplits order");
    }
    {   // Every slice corner projects inside its cascade's clip square [-1,1]^2 and depth [0,1], stable or not.
        for(bool stable : {true,false}) {
            const auto set=FitCascades({0,1.7f,0},Normalize(Vec3{0,0,1}),{0,1,0},std::tan(0.5411f),16.f/9,0.1f,100.f,sun,lo,hi,2048,stable);
            for(int c=0;c<4;++c) {
                const float n=c==0 ? 0.1f : set.cascades[c-1].splitFar, f=set.cascades[c].splitFar;
                for(int k=0;k<8;++k) {
                    const float d=(k&4) ? f : n, x=((k&1)?1:-1)*d*std::tan(0.5411f)*16.f/9, y=((k&2)?1:-1)*d*std::tan(0.5411f);
                    const Vec4 p=Mul(set.cascades[c].viewProjection,Vec4{x,1.7f+y,d,1});
                    Require(std::abs(p.x)<=1.0001f && std::abs(p.y)<=1.0001f && p.z>=-1e-4f && p.z<=1.0001f,"cascade covers its slice");
                }
            }
        }
    }
    {   // The blend band at the end of slice c (the shader's: blendFraction of [splitFar[c-1] or 0, splitFar[c]]) lies
        // inside cascade c+1 as well, stable or not, so the blend never samples outside the next map.
        for(bool stable : {true,false}) {
            const float tanY=std::tan(0.5411f), aspect=16.f/9;
            const auto set=FitCascades({0,1.7f,0},Normalize(Vec3{0.3f,-0.2f,1}),{0,1,0},tanY,aspect,0.1f,100.f,sun,lo,hi,2048,stable);
            const Vec3 fw=Normalize(Vec3{0.3f,-0.2f,1}), right=Normalize(Cross({0,1,0},fw)), up=Cross(fw,right);
            for(int c=0;c<3;++c) {
                const float start=c==0 ? 0 : set.cascades[c-1].splitFar, f=set.cascades[c].splitFar, band=(f-start)*set.blendFraction;
                for(int k=0;k<8;++k) {
                    const float d=(k&4) ? f : f-band, x=((k&1)?1:-1)*d*tanY*aspect, y=((k&2)?1:-1)*d*tanY;
                    const Vec3 p=Vec3{0,1.7f,0}+right*x+up*y+fw*d;
                    const Vec4 q=Mul(set.cascades[c+1].viewProjection,Vec4{p.x,p.y,p.z,1});
                    Require(std::abs(q.x)<=1.0001f && std::abs(q.y)<=1.0001f && q.z>=-1e-4f && q.z<=1.0001f,"blend band inside the next cascade");
                }
            }
        }
    }
    {   // Stability: moving the camera by a third of cascade 0's texel moves no cascade origin off its texel grid,
        // so the shadow-map texels map to the same world positions (snapped origin differences are whole texels).
        const auto a=FitCascades({0,1.7f,0},{0,0,1},{0,1,0},0.5f,16.f/9,0.1f,100.f,sun,lo,hi,2048,true);
        const float t=a.cascades[0].texelWorld/3;
        const auto b=FitCascades({t,1.7f,t},{0,0,1},{0,1,0},0.5f,16.f/9,0.1f,100.f,sun,lo,hi,2048,true);
        for(int c=0;c<4;++c) {
            const float dx=(b.cascades[c].viewProjection.m[0][3]-a.cascades[c].viewProjection.m[0][3])*1024; // clip offset in texels
            const float dy=(b.cascades[c].viewProjection.m[1][3]-a.cascades[c].viewProjection.m[1][3])*1024;
            Require(std::abs(dx-std::round(dx))<1e-2f && std::abs(dy-std::round(dy))<1e-2f,"stable cascades snap to texels");
            Require(std::abs(b.cascades[c].texelWorld-a.cascades[c].texelWorld)<1e-6f,"stable cascade size independent of view");
        }
    }
    {   // Degenerate views: forward parallel to the sun, or straight up/down: finite matrices.
        const Vec3 down=Normalize(Vec3{0,-1,0});
        for(Vec3 f : {Vec3{0,-1,0},Vec3{0,1,0}}) for(bool stable : {true,false}) {
            const auto s=FitCascades({0,1.7f,0},f,{0,0,1},0.5f,1.f,0.1f,100.f,down,lo,hi,2048,stable);
            for(const auto &c : s.cascades) for(auto &row : c.viewProjection.m) for(float v : row) Require(std::isfinite(v),"finite cascades");
        }
        const auto s=FitCascades({0,1.7f,0},sun,{0,1,0},0.5f,1.f,0.1f,100.f,sun,lo,hi,2048,true);
        for(const auto &c : s.cascades) for(auto &row : c.viewProjection.m) for(float v : row) Require(std::isfinite(v),"finite cascades, forward along the sun");
    }
    {   // Options.
        Require(RejectedWith({"bench","--csm"},"--csm requires --render raster"),"--csm requires --render raster");
        Require(RejectedWith({"bench","--render","raster","--csm-stable","0"},"--csm-stable requires --csm"),"--csm-stable requires --csm");
        Require(RejectedWith({"bench","--render","raster","--csm-distance","50"},"--csm-distance requires --csm"),"--csm-distance requires --csm");
        Require(RejectedWith({"bench","--render","raster","--ao","blurry"},"--ao clean|game"),"--ao clean|game");
        Require(RejectedWith({"bench","--ao","clean"},"--ao requires --render raster"),"--ao requires --render raster");
        Require(RejectedWith({"bench","--render","raster","--ao-radius","1"},"--ao-radius requires --ao"),"--ao-radius requires --ao");
        Require(RejectedWith({"bench","--render","raster","--ao-strength","1"},"--ao-strength requires --ao"),"--ao-strength requires --ao");
        Require(Rejected({"bench","--render","raster","--csm","--csm-distance","0"}) && Rejected({"bench","--render","raster","--ao","game","--ao-radius","0"}),"CSM/AO ranges");
        const Options o=Parse({"bench","--render","raster","--csm","--csm-stable","0","--csm-distance","60","--ao","game","--ao-radius","0.75","--ao-strength","2"});
        Require(o.csm && !o.csmStable && Near(o.csmDistance,60) && o.ao=="game" && Near(o.aoRadius,0.75f) && Near(o.aoStrength,2),"raster CSM/AO options parse");
        const Options d=Parse({"bench","--render","raster"});
        Require(!d.csm && d.csmStable && Near(d.csmDistance,100) && d.ao.empty() && Near(d.aoRadius,1) && Near(d.aoStrength,1),"CSM/AO defaults (AO strength 1 since 2026-10-07)");
    }
}
// --ao's helpers (ao_math.h, mirrored in shaders/raster_ao_math.hlsli).
void AoChecks()
{
    {   // Linear depth from the projection's depth, both conventions (math.h Perspective: n=0.1, f=300).
        for(bool reverse : {false,true}) for(float z : {0.1f,1.f,10.f,299.f}) {
            const Mat4 p=Perspective(62,16.f/9,reverse); const float d=(p.m[2][2]*z+p.m[2][3])/z;
            Require(std::abs(LinearDepth(d,reverse)-z)<1e-3f*z,"LinearDepth round trip");
        }
    }
    {   // Octahedral normal round trip within 1e-3.
        for(Vec3 n : {Vec3{0,0,1},Vec3{0,0,-1},Normalize(Vec3{1,2,-3}),Normalize(Vec3{-1,-1,0.01f})})
            Require(Length(OctDecode(OctEncode(n))-n)<2e-3f,"oct round trip");
    }
    {   // The sample pattern: any 4x4 window (the blur's, at any alignment) holds each of the 16 rotations and the 16
        // step offsets once, for every frame offset; rotation and step are not the same sequence.
        for(unsigned offset=0;offset<16;++offset) for(unsigned ox=0;ox<4;++ox) for(unsigned oy=0;oy<4;++oy) {
            bool rotations[16]={}, steps[16]={}; bool differ=false;
            for(unsigned y=0;y<4;++y) for(unsigned x=0;x<4;++x) {
                const AoInterleave p=AoPattern(ox+x+5,oy+y+9,offset);
                rotations[unsigned(p.rotation*16)]=true; steps[unsigned(p.step*16)]=true; differ|=p.rotation!=p.step;
            }
            Require(std::all_of(std::begin(rotations),std::end(rotations),[](bool b) { return b; })
                    && std::all_of(std::begin(steps),std::end(steps),[](bool b) { return b; }) && differ,"AO pattern covers a 4x4 window");
        }
        Require(AoPatternIndex(0,0,3)==3 && AoPatternIndex(1,0,0)==8 && AoPatternIndex(3,3,1)==6,"AO pattern index");
    }
    {   // --ao game's noise: in [0,1); a 4x4 window does not hold the same rotations on the next frame (the blur must not
        // integrate it away); rotation and step differ.
        unsigned changed=0, windows=0; bool inRange=true, differ=false;
        for(unsigned frame=0;frame<70;++frame) for(unsigned wy=0;wy<4;++wy) for(unsigned wx=0;wx<4;++wx) {
            float now[16], next[16];
            for(unsigned i=0;i<16;++i) {
                const unsigned x=wx*4+i%4+101, y=wy*4+i/4+37;
                const AoInterleave a=AoGameNoise(x,y,frame), b=AoGameNoise(x,y,frame+1);
                inRange&=a.rotation>=0 && a.rotation<1 && a.step>=0 && a.step<1; differ|=std::abs(a.rotation-a.step)>0.05f;
                now[i]=a.rotation; next[i]=b.rotation;
            }
            std::sort(now,now+16); std::sort(next,next+16);
            bool same=true; for(unsigned i=0;i<16;++i) same&=std::abs(now[i]-next[i])<1e-3f;
            changed+=same ? 0 : 1; ++windows;
        }
        Require(inRange && differ && changed==windows,"AO game noise changes per frame");
        Require(std::abs(AoIgn(0,0))<1e-6f && std::abs(AoGameNoise(3,5,64).rotation-AoGameNoise(3,5,0).rotation)<1e-6f,"AO game noise period");
    }
    {   // --ao game's temporal filter: the current AO alone when the history is invalid or rejected, else 10 % of it.
        Require(AoHistoryWeight(false,false)==1 && AoHistoryWeight(false,true)==1 && AoHistoryWeight(true,true)==1,"AO history weight, no history");
        Require(std::abs(AoHistoryWeight(true,false)-0.1f)<1e-6f,"AO history weight 0.1");
        Require(AoHistoryWeight(true,false,1)==1 && std::abs(AoHistoryWeight(true,false,0.3f)-0.3f)<1e-6f && AoHistoryWeight(false,false,0.3f)==1,"AO history weight --ao-alpha");
        // The history is usable only on the frame right after the last AO frame, on the same targets.
        Require(!AoHistoryUsable(true,-1,0) && !AoHistoryUsable(true,4,5),"AO history after (re)created targets");
        Require(AoHistoryUsable(false,4,5) && !AoHistoryUsable(false,4,4) && !AoHistoryUsable(false,4,7),"AO history across a frame gap");
    }
    {   // --ao game without --render raster only when --render-switch-at enters raster mode.
        Require(Parse({"bench","30","--ao","game","--render-switch-at","20"}).ao=="game","--ao with --render-switch-at into raster");
        // --ao-alpha: game only, 0 < A <= 1, default 0.1.
        Require(Near(Parse({"bench","--render","raster","--ao","game"}).aoAlpha,0.1f),"--ao-alpha default");
        Require(Near(Parse({"bench","--render","raster","--ao","game","--ao-alpha","1"}).aoAlpha,1),"--ao-alpha 1");
        Require(Near(Parse({"bench","--render","raster","--ao","game","--ao-alpha","0.3"}).aoAlpha,0.3f),"--ao-alpha 0.3");
        Require(RejectedWith({"bench","--render","raster","--ao","clean","--ao-alpha","0.5"},"--ao-alpha requires --ao game")
                && RejectedWith({"bench","--render","raster","--ao-alpha","0.5"},"--ao-alpha requires --ao game"),"--ao-alpha outside game");
        Require(Rejected({"bench","--render","raster","--ao","game","--ao-alpha","0"}) && Rejected({"bench","--render","raster","--ao","game","--ao-alpha","1.5"})
                && Rejected({"bench","--render","raster","--ao","game","--ao-alpha","nan"}),"--ao-alpha range");
    }
}
}
// The CSM and AO checks of tests_raster.cpp's RasterChecks.
void CsmAoChecks()
{
    CsmChecks();
    AoChecks();
}
