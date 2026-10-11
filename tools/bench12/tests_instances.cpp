// Instanced meshes, CPU checks: which nodes share a mesh as instances (scene_instances.h), their object-space geometry and
// transforms against the flattened vertices, the TLAS instances (accel_instances.h) and the raster culling (raster_instances.h).
#include "accel_instances.h"
#include "raster_instances.h"
#include "scene_instances.h"
#include "tests_raster_support.h"
#include <cmath>
#include <cstdio>
#include <iterator>
#include <string>
namespace {
using namespace raster_tests;
using pwgltf::Primitive;
void AddQuad(Primitive &p, float x0, float x1, float y0, float y1, float z0=0)
{
    const uint32_t base=uint32_t(p.positions.size());
    p.positions.insert(p.positions.end(),{{x0,y0,z0},{x1,y0,z0},{x1,y1,z0},{x0,y1,z0}});
    p.normals.insert(p.normals.end(),4,Vec3{0,0,1});
    p.indices.insert(p.indices.end(),{base,base+1,base+2,base,base+2,base+3});
}
pwgltf::Quat AxisAngle(Vec3 axis, float radians)
{
    const float s=std::sin(radians*0.5f); return {axis.x*s,axis.y*s,axis.z*s,std::cos(radians*0.5f)};
}
// Mesh 0 "crate": an alpha-tested quad then an opaque one (the opaque one must come first once instanced), used by nodes 0..2
// (moved, turned, scaled) and 3 (skinned: flattened), 4 (ground: its own one-node group, flattened). Mesh 1 "lamp": emissive,
// used twice (flattened). Mesh 2 "hall": used once (flattened).
pwgltf::Scene Yard()
{
    pwgltf::Scene s; s.meshes.resize(3);
    Primitive cut; cut.alphaMask=true; cut.roughness=0.7f; AddQuad(cut,0,1,0,1,0.5f);
    Primitive solid; solid.roughness=0.4f; AddQuad(solid,0,1,0,1);
    s.meshes[0].primitives={cut,solid};
    Primitive glow; glow.emission[0]=5; AddQuad(glow,0,1,0,1);
    s.meshes[1].primitives={glow};
    Primitive wall; AddQuad(wall,0,10,0,3);
    s.meshes[2].primitives={wall};
    const int meshes[]={0,0,0,0,0,1,1,2};
    s.nodes.resize(std::size(meshes));
    for(size_t i=0;i<s.nodes.size();++i) { s.nodes[i].mesh=meshes[i]; s.nodes[i].name="node"+std::to_string(i); s.roots.push_back(int(i)); }
    s.nodes[0].translation={2,0,-3};
    s.nodes[1].translation={-1,0.5f,4}; s.nodes[1].rotation=AxisAngle(Normalize({0.3f,1,0.2f}),1.1f);
    s.nodes[2].translation={5,1,1}; s.nodes[2].scale={2,0.5f,1.5f}; s.nodes[2].rotation=AxisAngle({0,0,1},0.4f);
    s.nodes[3].skin=0; s.skins.resize(1);
    s.nodes[4].isGround=true;
    return s;
}
bool Close(Vec3 a, Vec3 b, float tolerance=1e-5f) { return std::abs(a.x-b.x)<=tolerance && std::abs(a.y-b.y)<=tolerance && std::abs(a.z-b.z)<=tolerance; }
Vec3 Apply(const float rows[3][4], Vec3 p)
{
    return {rows[0][0]*p.x+rows[0][1]*p.y+rows[0][2]*p.z+rows[0][3],rows[1][0]*p.x+rows[1][1]*p.y+rows[1][2]*p.z+rows[1][3],
            rows[2][0]*p.x+rows[2][1]*p.y+rows[2][2]*p.z+rows[2][3]};
}
void PlanChecks()
{
    const auto s=Yard();
    const auto plan=PlanInstances(s,true);
    Require(plan.groups.size()==1 && plan.groups[0].mesh==0,"one group: the crate mesh");
    Require(plan.groups[0].nodes==std::vector<size_t>({0,1,2}),"the crate's static unskinned non-ground nodes are its instances");
    for(size_t i=0;i<s.nodes.size();++i) Require(plan.instanced[i]==(i<3),"only the group's nodes leave the flattened list");
    Require(PlanInstances(s,false).groups.empty(),"--no-instancing or --lightmap: no groups");
    auto moving=Yard(); moving.channels.push_back({}); moving.channels.back().node=1; moving.channels.back().path=0;
    const auto animated=PlanInstances(moving,true);
    Require(animated.groups.size()==1 && animated.groups[0].nodes==std::vector<size_t>({0,2}),"an animated node stays flattened");
}
void GeometryChecks()
{
    const auto s=Yard();
    const auto plan=PlanInstances(s,true);
    const auto g=BuildInstancedGeometry(s,plan,100);
    Require(g.meshes.size()==1 && g.meshes[0].firstTriangle==100 && g.meshes[0].triangles==4 && g.meshes[0].opaqueTriangles==2,
            "the crate is stored once: 4 triangles from 100, 2 opaque");
    Require(g.meshes[0].firstInstance==0 && g.meshes[0].instances==3 && g.instances.size()==3 && g.boxes.size()==3,"three instances");
    Require(g.vertices.size()==12 && g.roughness.size()==4,"3 vertices and a roughness per stored triangle");
    Require(!g.vertices[0].alphaMask && g.vertices[6].alphaMask && Near(g.roughness[0],0.4f) && Near(g.roughness[2],0.7f),"opaque triangles first");
    for(size_t k=0;k<g.instances.size();++k) {
        const auto &instance=g.instances[k];
        Require(instance.firstTriangle==100 && instance.opaqueTriangles==2,"the instance names its mesh's triangles");
        // The flattened vertices of the same node (BuildVerticesSubset, solid primitive first as stored) against rows * object.
        std::vector<pwgltf::Vertex> flat; std::vector<Vec3> previous; pwgltf::VertexWorkspace scratch;
        pwgltf::BuildVerticesSubset(s,0,0,{{plan.groups[0].nodes[k],1},{plan.groups[0].nodes[k],0}},scratch,flat,previous);
        Require(flat.size()==g.vertices.size(),"same vertex count as flattened");
        Vec3 lo=flat[0].pos, hi=flat[0].pos;
        for(size_t v=0;v<flat.size();++v) {
            Require(Close(Apply(instance.world,g.vertices[v].pos),flat[v].pos),"world rows * object position == flattened position");
            const Vec3 n=Normalize(Vec3{Apply(instance.world,g.vertices[v].normal)-Apply(instance.world,{})});
            Require(Close(n,flat[v].normal),"world rows * object normal, normalised == flattened normal");
            Require(Close(Apply(instance.previous,g.vertices[v].pos),flat[v].pos),"static: the previous transform is the world one");
            lo={std::min(lo.x,flat[v].pos.x),std::min(lo.y,flat[v].pos.y),std::min(lo.z,flat[v].pos.z)};
            hi={std::max(hi.x,flat[v].pos.x),std::max(hi.y,flat[v].pos.y),std::max(hi.z,flat[v].pos.z)};
        }
        Require(Close(g.boxes[k].lo,lo,0) && Close(g.boxes[k].hi,hi,0),"the instance bounds are the flattened vertices' exactly");
    }
    Require(BuildInstancedGeometry(s,PlanInstances(s,false),0).meshes.empty(),"no plan, no instanced geometry");
}
void TopChecks()
{
    const auto s=Yard();
    const auto g=BuildInstancedGeometry(s,PlanInstances(s,true),100);
    const auto all=TopInstances(g.meshes,g.instances,0x1000,0x2000,{0x3000});
    Require(all.size()==5,"TLAS: static, dynamic and 3 instances");
    Require(all[0].InstanceID==0 && all[1].InstanceID==1 && all[2].InstanceID==2 && all[4].InstanceID==4,"InstanceID 2 + SceneInstance");
    Require(all[0].Transform[0][0]==1 && all[0].Transform[0][3]==0 && all[2].AccelerationStructure==0x3000,"identity static, mesh BLAS");
    for(unsigned k=0;k<3;++k) for(int r=0;r<3;++r) for(int c=0;c<4;++c)
        Require(all[2+k].Transform[r][c]==g.instances[k].world[r][c],"the TLAS transform is the instance's world rows");
    Require(TopInstances(g.meshes,g.instances,0x1000,0,{0x3000}).size()==4,"the probes' structure: no dynamic BLAS");
    Require(TopInstances({},{},0x1000,0x2000,{}).size()==2,"without instancing: the two BLAS");
}
void CullChecks()
{
    Mat4 ortho=Identity(); // clip = world: the pass sees |x|, |y| <= 1
    Require(!BoxOutside(ortho,{-0.5f,-0.5f,0},{0.5f,0.5f,1},0),"inside");
    Require(!BoxOutside(ortho,{0.9f,-3,0},{2,3,1},0),"straddling an edge");
    Require(BoxOutside(ortho,{1.1f,0,0},{2,0.5f,1},0) && BoxOutside(ortho,{0,-3,0},{0.5f,-1.2f,1},0),"beyond the right or lower edge");
    Require(!BoxOutside(ortho,{1.01f,0,0},{2,0.5f,1},0.02f),"the margin widens the planes");
    const Mat4 view=Mul(Perspective(60,1.5f,false),LookAt({0,0,0},{0,0,1}));
    Require(!BoxOutside(view,{-1,-1,4},{1,1,6},0),"in front of the camera");
    Require(BoxOutside(view,{-1,-1,-6},{1,1,-4},0),"behind the camera");
    Require(BoxOutside(view,{20,-1,4},{22,1,6},0),"off to the side");
    Require(Parse({"bench","--no-instancing"}).instancing==false && Parse({"bench"}).instancing,"--no-instancing");
}
}
void InstanceChecks() { PlanChecks(); GeometryChecks(); TopChecks(); CullChecks(); std::puts("[test] instancing checks passed"); }
