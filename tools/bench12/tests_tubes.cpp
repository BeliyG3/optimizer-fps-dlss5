#include "tests_raster_support.h"
#include "lamp_parts.h"
#include "raster_lights.h"
#include <vector>
using namespace raster_tests;
namespace {
bool Within(float value, float expected, float tolerance) { return std::abs(value-expected)<=tolerance*std::abs(expected); }
// An emissive box (12 triangles, glTF-style: every face has its own vertices, the corners share positions) centred at
// `centre`, `size` along the axes u, v and u x v, appended to `sources`/`triangles` in `group`.
void AddBox(std::vector<LampSource> &sources, std::vector<LampTriangle> &triangles, unsigned group, Vec3 centre, Vec3 size,
            Vec3 u={1,0,0}, Vec3 v={0,1,0}, Vec3 emission={1,1,1})
{
    const Vec3 w=Cross(u,v);
    auto corner=[&](int x, int y, int z) { return centre+u*(0.5f*size.x*float(x))+v*(0.5f*size.y*float(y))+w*(0.5f*size.z*float(z)); };
    const int faces[6][4][3]={{{-1,-1,-1},{-1,1,-1},{-1,1,1},{-1,-1,1}},{{1,-1,-1},{1,-1,1},{1,1,1},{1,1,-1}},
        {{-1,-1,-1},{-1,-1,1},{1,-1,1},{1,-1,-1}},{{-1,1,-1},{1,1,-1},{1,1,1},{-1,1,1}},
        {{-1,-1,-1},{1,-1,-1},{1,1,-1},{-1,1,-1}},{{-1,-1,1},{-1,1,1},{1,1,1},{1,-1,1}}};
    for(const auto &f:faces) {
        const Vec3 q[4]={corner(f[0][0],f[0][1],f[0][2]),corner(f[1][0],f[1][1],f[1][2]),corner(f[2][0],f[2][1],f[2][2]),corner(f[3][0],f[3][1],f[3][2])};
        for(const LampTriangle t:{LampTriangle{q[0],q[1],q[2]},LampTriangle{q[0],q[2],q[3]}}) {
            const float area=0.5f*Length(Cross(t[1]-t[0],t[2]-t[0]));
            sources.push_back({unsigned(sources.size()),group,area,emission}); triangles.push_back(t);
        }
    }
}
std::vector<Vec3> Centroids(const std::vector<LampTriangle> &triangles)
{
    std::vector<Vec3> c; for(const auto &t:triangles) c.push_back((t[0]+t[1]+t[2])*(1.0f/3)); return c;
}
// The irradiance of a line light of total intensity 1, cos-weighted to its face normal m, by summing many point lights.
float NumericLine(Vec3 p, Vec3 n, Vec3 a, Vec3 b, Vec3 m, bool twoSided)
{
    const int steps=20000; double sum=0;
    for(int i=0;i<steps;++i) {
        const Vec3 x=a+(b-a)*((float(i)+0.5f)/steps), d=x-p; const float r2=Dot(d,d), r=std::sqrt(r2);
        float face=-Dot(m,d)/r; face=twoSided ? std::abs(face) : std::max(face,0.0f);
        sum+=face*std::max(0.0f,Dot(n,d))/(r2*r);
    }
    return float(sum/steps);
}
}
// --lamp-tubes: the connected parts and their axes, the power split, the line light's closed-form irradiance.
void TubeChecks()
{
    const Vec3 down{0,-1,0};
    {   // A flat 8 x 0.03 x 0.1 strip along x (the thin axis vertical) is one downward tube: the long axis, its full length,
        // half its mean thickness, its face normal -y.
        std::vector<LampSource> sources; std::vector<LampTriangle> triangles;
        AddBox(sources,triangles,0,{1,2,3},{8,0.03f,0.1f});
        const auto parts=FindLampParts(sources,triangles);
        Require(parts.size()==1 && parts[0].tube && parts[0].sources.size()==12,"strip: one tube part of 12 triangles");
        const Vec3 axis=parts[0].b-parts[0].a;
        Require(Within(Length(axis),8,1e-3f) && Within(std::abs(axis.x),8,1e-3f),"strip: the long axis and its length");
        Require(Within(parts[0].radius,0.0325f,1e-2f),"strip: radius is half the mean thickness");
        Require(Length((parts[0].a+parts[0].b)*0.5f-Vec3{1,2,3})<1e-3f,"strip: the axis through the centre");
        Require(!parts[0].twoSided && Dot(parts[0].normal,down)>0.9999f,"strip: one-sided, emitting downward");
    }
    {   // Rotated off the axes (the long side along (1,0,1), the wide side horizontal): PCA finds the axis and the face.
        std::vector<LampSource> sources; std::vector<LampTriangle> triangles;
        const Vec3 u=Normalize({1,0,1}), v=Normalize({-1,0,1});
        AddBox(sources,triangles,0,{-2,4,1},{2.4f,0.18f,0.06f},u,v); // the third axis u x v is vertical
        const auto parts=FindLampParts(sources,triangles);
        Require(parts.size()==1 && parts[0].tube,"rotated fixture: one tube part");
        const Vec3 axis=parts[0].b-parts[0].a;
        Require(Within(Length(axis),2.4f,1e-3f) && std::abs(Dot(Normalize(axis),u))>0.9999f,"rotated fixture: the diagonal axis");
        Require(Within(parts[0].radius,0.06f,1e-2f) && Dot(parts[0].normal,down)>0.9999f,"rotated fixture: radius and face");
    }
    {   // A standing sign (2 x 0.33 x 0.05, the thin axis horizontal) is two-sided. Without a dominant face (8 x 0.1 x 0.1,
        // a square-section rod) and short parts (a spot, a screen) stay compact. Parts never span groups.
        std::vector<LampSource> sources; std::vector<LampTriangle> triangles;
        AddBox(sources,triangles,0,{0,0,0},{0.35f,0.35f,0.04f});
        AddBox(sources,triangles,0,{5,0,0},{0.9f,0.03f,0.55f});
        AddBox(sources,triangles,1,{0,0,0},{2,0.33f,0.05f});
        AddBox(sources,triangles,1,{0,5,0},{8,0.1f,0.1f});
        const auto parts=FindLampParts(sources,triangles);
        Require(parts.size()==4,"four parts: two of group 0, two of group 1");
        Require(!parts[0].tube && !parts[1].tube && !parts[3].tube,"spot, screen and square rod compact");
        Require(parts[2].tube && parts[2].group==1 && parts[2].twoSided && std::abs(parts[2].normal.z)>0.9999f,"the sign a two-sided tube");
    }
    {   // The power split: two tubes and a compact box in one group (intensity 2, tint), a lone compact box in another.
        // Each tube's power is its emitting area's share; lines plus points sum to the group power of BuildRasterLights.
        std::vector<LampSource> sources; std::vector<LampTriangle> triangles;
        AddBox(sources,triangles,0,{0,3,0},{8,0.03f,0.1f});
        AddBox(sources,triangles,0,{0,3,4},{2,0.03f,0.1f});
        AddBox(sources,triangles,0,{6,3,0},{0.35f,0.35f,0.04f});
        AddBox(sources,triangles,1,{-6,1,0},{0.9f,0.03f,0.55f},{1,0,0},{0,1,0},{0.3f,0.6f,1});
        std::vector<LampGroup> groups(2); groups[0].name="tubes"; groups[0].intensity=2; groups[0].tint={1,0.5f,0.25f}; groups[1].name="screen";
        const auto centroids=Centroids(triangles);
        const auto parts=FindLampParts(sources,triangles);
        const auto lamps=BuildRasterLampTubes(sources,centroids,groups,parts);
        const auto reference=BuildRasterLights(sources,centroids,groups);
        Require(lamps.lines.size()==2 && lamps.points.size()==2,"two lines, a point per group for its compact parts");
        Vec3 total{}; for(const auto &l:lamps.lines) total=total+l.power; for(const auto &p:lamps.points) total=total+p.power;
        Vec3 expected{}; for(const auto &p:reference) expected=expected+p.power;
        Require(Within(total.x,expected.x,1e-5f) && Within(total.y,expected.y,1e-5f) && Within(total.z,expected.z,1e-5f),
            "lines and points sum to the groups' power");
        float area[2]={};
        for(size_t i=0;i<2;++i) for(unsigned s:parts[i].sources) area[i]+=sources[s].area;
        Require(Within(lamps.lines[0].power.x/lamps.lines[1].power.x,area[0]/area[1],1e-5f),"tube power by emitting area");
        Require(Within(lamps.lines[0].power.y,0.5f*lamps.lines[0].power.x,1e-5f),"tube power carries the tint");
        Require(Near(lamps.points[1].power.x,reference[1].power.x) && Near(lamps.points[1].position.x,reference[1].position.x),
            "a group without tubes keeps its point light");
        Require(lamps.lines[0].twoSided==0 && Near(lamps.lines[0].normal.y,-1) && Near(lamps.lines[0].range,TubeRange),
            "the line carries its face and range");
    }
    {   // The closed form: exact against a numeric sum along the segment, one- and two-sided, with and without the horizon.
        const Vec3 a{-0.7f,2.5f,0}, b{0.7f,2.5f,0}, side{0,0,1};
        const Vec3 cases[][2]={{{0,0,0},{0,1,0}},{{3,0,1},{0,1,0}},{{0.2f,2.4f,0.3f},Normalize({1,0.2f,0})},{{1,1,-1},Normalize({-1,1,1})}};
        for(const auto &c:cases) {
            Require(Within(LineIrradiance(c[0],c[1],a,b,down,false,0.001f),NumericLine(c[0],c[1],a,b,down,false),2e-3f),
                "one-sided line irradiance matches the numeric integral");
            Require(Within(LineIrradiance(c[0],c[1],a,b,side,true,0.001f),NumericLine(c[0],c[1],a,b,side,true),2e-3f),
                "two-sided line irradiance matches the numeric integral");
        }
        Require(LineIrradiance({0,0,0},{0,-1,0},a,b,down,false,0.03f)==0,"a line below the receiver's horizon gives nothing");
        Require(LineIrradiance({0,4,0},{0,1,0},a,b,down,false,0.03f)==0 && LineIrradiance({3,2.5f,1},{-1,0,0},a,b,down,false,0.03f)==0,
            "nothing above the emitting plane or level with it");
    }
    {   // Far from a short tube: Lambertian toward the face normal (one face of the part's area), within 2 %.
        const Vec3 a{-0.1f,3,0}, b{0.1f,3,0}, centre{0,3,0};
        for(const Vec3 p:{Vec3{0,-7,0},Vec3{4,-5,3},Vec3{-6,1,-2}}) {
            const Vec3 n=Normalize(centre-p+Vec3{0.3f,0,0.2f}), d=centre-p;
            const float point=Dot(n,Normalize(d))/Dot(d,d)*Dot(down,Normalize(p-centre));
            Require(Within(LineIrradiance(p,n,a,b,down,false,0.02f),point,0.02f),"far from a short tube: a Lambertian point within 2 %");
        }
        // A lab fixture (2.4 x 0.06 x 0.18 box, the thin axis vertical): its face and sides as Lambertian faces of the tube's
        // radiance. Far away the line is the point source of those faces; nothing above the face's plane.
        std::vector<LampSource> sources; std::vector<LampTriangle> triangles;
        AddBox(sources,triangles,0,{0,6,0},{2.4f,0.06f,0.18f});
        std::vector<LampGroup> groups(1); groups[0].name="bay";
        const auto lamps=BuildRasterLampTubes(sources,Centroids(triangles),groups,FindLampParts(sources,triangles));
        Require(lamps.lines.size()==1,"fixture: one line");
        const RasterLine &line=lamps.lines[0]; float area=0; for(const auto &source:sources) area+=source.area;
        Require(Within(line.faceShare,2.4f*0.18f/area,1e-3f) && Within(line.sideShare,2.4f*0.06f/area,1e-3f),"fixture: face and side shares");
        for(const Vec3 p:{Vec3{0,-20,0},Vec3{15,-14,6},Vec3{-9,-25,-12}}) {
            const Vec3 n=Normalize(Vec3{0,6,0}-p+Vec3{1,0,0.5f}), d=Vec3{0,6,0}-p, l=Normalize(d);
            const float faces=line.faceShare*std::max(0.0f,-Dot(down,l))+line.sideShare*std::abs(l.z); // the side axis is z
            Require(Within(TubeIrradiance(p,n,line),faces*Dot(n,l)/Dot(d,d),0.02f),"fixture far field: its faces as a point within 2 %");
        }
        Require(TubeIrradiance({3,6.5f,0},{0,-1,0},line)==0,"fixture: nothing above its face's plane, sides included");
    }
    {   // Under the middle of a long tube the irradiance exceeds that under its end; close to the axis it is held at the radius.
        const Vec3 a{-4,2,0}, b{4,2,0}, up{0,1,0};
        const float middle=LineIrradiance({0,0,0},up,a,b,down,false,0.05f), end=LineIrradiance({4,0,0},up,a,b,down,false,0.05f);
        Require(middle>1.5f*end && end>0,"under the middle brighter than under the end");
        const float touching=LineIrradiance({0,1.999f,0},up,a,b,down,false,0.05f), atRadius=LineIrradiance({0,1.95f,0},up,a,b,down,false,0.05f);
        Require(std::isfinite(touching) && touching<=atRadius*1.01f,"held at the tube radius");
    }
    {   // The range: 0.5 % of the value at 1 m by the inverse square, faded out over its last quarter.
        Require(Within(TubeRange,14.142f,1e-3f),"range sqrt(200) m");
        Require(TubeRangeWindow(0,TubeRange)==1 && TubeRangeWindow(0.74f*0.74f*TubeRange*TubeRange,TubeRange)==1,"no fade within 0.75 range");
        const float halfway=TubeRangeWindow(0.875f*0.875f*TubeRange*TubeRange,TubeRange);
        Require(halfway>0.2f && halfway<0.8f && TubeRangeWindow(TubeRange*TubeRange,TubeRange)==0,"faded to 0 at the range");
    }
    Require(Parse({"bench","--render","raster","--lamp-tubes"}).lampTubes,"--lamp-tubes parses with --render raster");
    Require(RejectedWith({"bench","--lamp-tubes"},"--lamp-tubes requires --render raster"),"--lamp-tubes rejected without raster");
    std::puts("[test] tube checks passed");
}
