// Culling of small instances: the wide cone (raster_cull.h) and the Hi-Z test (hiz_math.h = shaders/raster_occlusion.hlsl),
// which must be conservative: never occluded while any pixel its box may cover has an occluder depth behind the box.
#include "hiz_math.h"
#include "raster_cull.h"
#include "scene_instances.h"
#include <random>
#include <stdexcept>

namespace {
void Require(bool value, const char *message) { if(!value) throw std::runtime_error(message); }
void ConeChecks()
{
    const Vec3 eye{0,0,0}, forward{0,0,1};
    Require(!SphereOutsideCone(eye,forward,170,0,{0,0,5},0.5f),"Ahead is inside the cone");
    Require(!SphereOutsideCone(eye,forward,170,0,{5,0,0},0.5f),"90 degrees off the view is inside a 170 degree cone");
    Require(!SphereOutsideCone(eye,forward,170,0,{5,0,-0.05f},0.9f),"A sphere straddling the cone's edge stays");
    Require(SphereOutsideCone(eye,forward,170,0,{0,0,-5},0.5f),"Behind the camera is outside");
    Require(SphereOutsideCone(eye,forward,170,0,{5,0,-1.5f},0.5f),"Past the edge by more than the radius is outside");
    Require(!SphereOutsideCone(eye,forward,170,0,{0,0,-0.3f},0.5f),"A sphere around the eye is never outside");
    Require(SphereOutsideCone(eye,forward,62,0,{5,0,1},0.5f) && !SphereOutsideCone(eye,forward,62,0,{1,0,5},0.5f),"A narrow cone");
    Require(SphereOutsideCone(eye,forward,170,10,{0,0,12},0.5f) && !SphereOutsideCone(eye,forward,170,10,{0,0,10.4f},0.5f),"The distance limit");
    Require(!SphereOutsideCone(eye,forward,360,0,{0,0,-5},0.5f),"A 360 degree cone keeps everything");
    // Exhaustive against sampled points: a sphere declared outside has no point inside the cone.
    std::mt19937 random(7); std::uniform_real_distribution<float> u(-1,1);
    for(int t=0;t<2000;++t) {
        const Vec3 centre{u(random)*6,u(random)*6,u(random)*6}; const float radius=0.05f+0.7f*(u(random)+1);
        if(!SphereOutsideCone(eye,forward,170,0,centre,radius)) continue;
        for(int k=0;k<200;++k) {
            Vec3 p{u(random),u(random),u(random)}; if(Dot(p,p)>1) continue;
            const Vec3 q=centre+p*radius; const float length=Length(q);
            Require(length<1e-6f || Dot(q,forward)/length<std::cos(85.0f*3.14159265f/180)+1e-4f,"Outside spheres have no point in the cone");
        }
    }
    SmallCull cull; cull.radius=0.75f;
    std::vector<InstanceBox> boxes={{{-0.2f,0,-0.2f},{0.2f,0.5f,0.2f}},{{-3,0,-3},{3,8,3}},{{10,0,-10.5f},{10.5f,0.5f,-10}}};
    cull.Classify(boxes);
    Require(cull.smallCount==2 && cull.smallMask[0] && !cull.smallMask[1] && cull.smallMask[2],"Small instances by their bounding sphere");
    cull.Update({0,1,0},{1,0,0}); // looking along +x: the third box is ahead, the first one around the eye
    Require(cull.droppedCount==0,"Nothing small outside the cone");
    cull.Update({0,1,-20},{0,0,-1}); // looking away from both small boxes
    Require(cull.dropped[0] && !cull.dropped[1] && cull.dropped[2],"Small instances behind the camera are dropped, large ones never");
}
void HiZChecks()
{
    const unsigned width=97, height=61; // odd sizes: the clamped last texels
    const Mat4 viewProjection=Mul(Perspective(62,float(width)/float(height),false),LookAt({0,1,0},{0,1,1}));
    const Mat4 reverseProjection=Mul(Perspective(62,float(width)/float(height),true),LookAt({0,1,0},{0,1,1}));
    std::mt19937 random(3); std::uniform_real_distribution<float> u(0,1);
    unsigned occluded=0, tested=0;
    for(int reverse=0;reverse<2;++reverse) for(int scene=0;scene<30;++scene) {
        const Mat4 &vp=reverse ? reverseProjection : viewProjection;
        // Depth: a wall at a random distance with holes and a few nearer blocks (as an occluder pass draws).
        std::vector<float> depth(size_t(width)*height);
        const float wall=2+u(random)*20;
        auto ndc=[&](float distance) { const Vec4 c=Mul(vp,Vec4{0,1,distance,1}); return c.z/c.w; };
        for(unsigned y=0;y<height;++y) for(unsigned x=0;x<width;++x) {
            float d=(x>60 && x<68 && y>20 && y<40) || u(random)<0.0005f ? (reverse ? 0.0f : 1.0f) : ndc(wall*(0.9f+0.2f*u(random)));
            if(x>20 && x<35 && y>10 && y<30) d=ndc(wall*0.5f);
            depth[size_t(y)*width+x]=d;
        }
        const HiZ z=BuildHiZ(depth,width,height,reverse!=0);
        for(int b=0;b<300;++b) {
            const Vec3 c{(u(random)-0.5f)*wall,1+(u(random)-0.5f)*wall*0.6f,0.05f+u(random)*wall*1.6f};
            const float r=0.05f+u(random)*0.6f;
            const Vec3 lo{c.x-r,c.y-r,c.z-r}, hi{c.x+r,c.y+r,c.z+r};
            ++tested;
            if(!HiZOccluded(z,vp,lo,hi,reverse!=0)) continue;
            ++occluded;
            // Brute force: every pixel the box's corners project into (plus the margin) has an occluder in front of the box.
            float minX=1e30f, maxX=-1e30f, minY=1e30f, maxY=-1e30f, nearest=reverse ? 0.0f : 1.0f;
            for(unsigned k=0;k<8;++k) {
                const Vec4 q=Mul(vp,Vec4{k&1 ? hi.x : lo.x,k&2 ? hi.y : lo.y,k&4 ? hi.z : lo.z,1});
                Require(q.w>=0.1f,"Occluded boxes are in front of the near plane");
                const float x=(q.x/q.w*0.5f+0.5f)*width, y=(0.5f-q.y/q.w*0.5f)*height, d=q.z/q.w;
                minX=std::min(minX,x); maxX=std::max(maxX,x); minY=std::min(minY,y); maxY=std::max(maxY,y);
                nearest=reverse ? std::max(nearest,d) : std::min(nearest,d);
            }
            for(int y=std::max(0,int(std::floor(minY-1)));y<=std::min(int(height)-1,int(std::floor(maxY+1)));++y)
                for(int x=std::max(0,int(std::floor(minX-1)));x<=std::min(int(width)-1,int(std::floor(maxX+1)));++x) {
                    const float d=depth[size_t(y)*width+x];
                    Require(reverse ? d>nearest : d<nearest,"Hi-Z occlusion is conservative: no pixel of the footprint shows the box");
                }
        }
    }
    Require(occluded>tested/10,"The Hi-Z test culls boxes behind the wall");
    std::printf("[test] culling: cone checks passed; Hi-Z: %u of %u random boxes occluded, all conservative\n",occluded,tested);
}
}
void CullingChecks() { ConeChecks(); HiZChecks(); }
