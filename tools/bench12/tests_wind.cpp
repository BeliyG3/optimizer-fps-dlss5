// --wind CPU checks: the weights, the displacement, which nodes become dynamic, and the vertices Animation writes.
#include "animation.h"
#include "tests_raster_support.h"
#include "wind.h"
#include <cmath>
#include <cstdio>
#include <stdexcept>
namespace {
using namespace raster_tests;
using pwgltf::Node; using pwgltf::Primitive;
// A quad from (x0, y0) to (x1, y1) in the plane z = z0, two triangles, four vertices.
void AddQuad(Primitive &p,float x0,float x1,float y0,float y1,float z0=0)
{
    const uint32_t base=uint32_t(p.positions.size());
    p.positions.insert(p.positions.end(),{{x0,y0,z0},{x1,y0,z0},{x1,y1,z0},{x0,y1,z0}});
    p.indices.insert(p.indices.end(),{base,base+1,base+2,base,base+2,base+3});
}
Primitive Prim(const char *material="") { Primitive p; p.materialName=material; return p; }
// 0 veg_grass: two quads at different heights, one merged mesh   1 veg_tree: a trunk and a leaves primitive, 0..10 m
// 2 rock: not foliage
pwgltf::Scene Garden()
{
    pwgltf::Scene s;
    Primitive grass=Prim("grass"); AddQuad(grass,0,1,0,0.4f); AddQuad(grass,3,4,5,5.5f,2);
    Primitive trunk=Prim("island_tree_02"); AddQuad(trunk,10,11,0,10);
    Primitive leaves=Prim("island_tree_02_leaves"); AddQuad(leaves,10,11,0,10,1); AddQuad(leaves,10,11,5,10,1);
    Primitive rock=Prim("stone"); AddQuad(rock,20,21,0,1);
    s.meshes.resize(3); s.meshes[0].primitives={grass}; s.meshes[1].primitives={trunk,leaves}; s.meshes[2].primitives={rock};
    s.nodes.resize(3); s.nodes[0].name="veg_grass_a"; s.nodes[1].name="veg_tree_01"; s.nodes[2].name="rock";
    for(int i=0;i<3;++i) { s.nodes[size_t(i)].mesh=i; s.roots.push_back(i); }
    return s;
}
bool SameVec(Vec3 a,Vec3 b) { return Near(a.x,b.x) && Near(a.y,b.y) && Near(a.z,b.z); }
void OffsetChecks()
{
    const Vec3 rest{2,1,3};
    const Vec3 a=WindOffset(0,rest,1,1), b=WindOffset(1,rest,1,1);
    Require(!SameVec(a,b),"WindOffset at frames 0 and 1 differs");
    Require(SameVec(WindOffset(7,rest,0.6f,1.5f),WindOffset(7,rest,0.6f,1.5f)),"WindOffset is deterministic");
    const Vec3 zero=WindOffset(5,rest,0,1); Require(zero.x==0 && zero.y==0 && zero.z==0,"WindOffset is 0 at weight 0");
    const Vec3 still=WindOffset(5,rest,1,0); Require(still.x==0 && still.z==0,"WindOffset is 0 at strength 0");
    float largest=0;
    for(unsigned frame=0;frame<2000;frame+=7) for(float x=-30;x<30;x+=1.7f) {
        const Vec3 o=WindOffset(frame,{x,0,x*0.3f+2},1,1); largest=std::max(largest,Length(o));
        Require(Near(o.y,0) && Near(o.x*kWindDirZ,o.z*kWindDirX),"WindOffset is horizontal along the wind");
    }
    Require(largest<=(0.08f+0.03f)+1e-4f && largest>0.08f,"WindOffset magnitude is within (0.08 + 0.03) S and reaches past 0.08 S");
    Require(Near(Length(WindOffset(3,rest,1,2.5f)),2.5f*Length(WindOffset(3,rest,1,1))),"WindOffset scales with the strength");
    Require(Near(Length(WindOffset(3,rest,0.25f,1)),0.25f*Length(WindOffset(3,rest,1,1))),"WindOffset scales with the weight");
    // The formula itself, at frame 30 (t = 0.5 s): p = 2 * 0.8 + 3 * 0.6 = 3.4
    const double t=0.5, p=3.4, sway=0.08*std::sin(3.7699112*t+0.35*p)+0.03*std::sin(2.7*3.7699112*t+1.3*0.35*p);
    Require(Near(WindOffset(30,rest,1,1).x,float(0.8*sway)) && Near(WindOffset(30,rest,1,1).z,float(0.6*sway)),"WindOffset equals the formula");
}
void WeightChecks()
{
    const pwgltf::Scene s=Garden();
    // Grass: quads at 0..0.4 and 5..5.5 m in one node; the bottom vertices (positions 0, 1) get 0, the top vertices (2, 3) get 1.
    const auto grass=WindWeights(s,0,0);
    Require(grass.size()==8,"grass weights per vertex");
    for(size_t quad=0;quad<2;++quad) for(size_t k=0;k<4;++k)
        Require(grass[quad*4+k]==(k<2 ? 0.0f : 1.0f),"a grass quad's bottom vertices have weight 0 and its top vertices 1");
    // The per-vertex form on a single quad node
    pwgltf::Scene one; one.nodes.resize(1); one.nodes[0].name="veg_grass"; one.nodes[0].mesh=0; one.roots={0};
    one.meshes.resize(1); one.meshes[0].primitives={Prim("grass")}; AddQuad(one.meshes[0].primitives[0],0,1,0,0.5f);
    Require(WindWeight(one,0,{0,0,0},"grass")==0 && WindWeight(one,0,{1,0.5f,0},"grass")==1,"WindWeight: grass bottom 0, top 1");
    // Tree: trunk material 0; leaves (height above the base / height)^2 over 0..10 m
    const auto trunk=WindWeights(s,1,0);
    for(float w:trunk) Require(w==0,"the trunk material has weight 0");
    Require(WindWeight(s,1,{10,10,0},"island_tree_02")==0 && WindWeight(s,1,{10,10,0},"tree_trunk")==0,"WindWeight: trunk material 0");
    const auto leaves=WindWeights(s,1,1);
    Require(leaves.size()==8 && Near(leaves[0],0) && Near(leaves[1],0) && Near(leaves[2],1) && Near(leaves[3],1),"tree top weight 1, base 0");
    Require(Near(leaves[4],0.25f) && Near(leaves[5],0.25f) && Near(leaves[6],1),"half height weighs a quarter (squared)");
    Require(Near(WindWeight(s,1,{10,5,0},"island_tree_02_leaves"),0.25f) && Near(WindWeight(s,1,{10,2,0},"island_tree_02_leaves"),0.04f),"WindWeight: squared height");
    Require(IsTrunkMaterial("Island_Tree_02") && IsTrunkMaterial("oak_bark") && IsTrunkMaterial("trunk.001") &&
        !IsTrunkMaterial("island_tree_02_leaves") && !IsTrunkMaterial("island_tree_02_branches") && !IsTrunkMaterial("fern_02"),"trunk material names");
}
unsigned DynamicCount(const pwgltf::Scene &s) { unsigned n=0; for(size_t i=0;i<s.nodes.size();++i) n+=pwgltf::DynamicNode(s,i); return n; }
void NodeChecks()
{
    pwgltf::Scene s=Garden();
    Require(DynamicCount(s)==0,"foliage is static without --wind");
    Require(MarkWindNodes(s)==2 && s.nodes[0].wind && s.nodes[1].wind && !s.nodes[2].wind,"only veg_ nodes get the wind flag");
    Require(pwgltf::DynamicNode(s,0) && pwgltf::DynamicNode(s,1) && !pwgltf::DynamicNode(s,2),"veg_ nodes are dynamic with the flag");
    Require(pwgltf::Primitives(s,1).size()==3 && pwgltf::Primitives(s,0).size()==1,"wind moves the veg_ primitives to the dynamic list");
    // No veg_ nodes: --wind adds no dynamic triangles
    pwgltf::Scene lab=Garden(); lab.nodes[0].name="grass"; lab.nodes[1].name="tree"; lab.nodes[2].name="veg"; // none starts with "veg_"
    const auto before=pwgltf::Primitives(lab,1).size();
    Require(MarkWindNodes(lab)==0 && DynamicCount(lab)==0 && pwgltf::Primitives(lab,1).size()==before,"no veg_ nodes: wind adds no dynamic triangles");
    // A veg_ node without a mesh is not a wind node
    pwgltf::Scene empty=Garden(); empty.nodes[0].mesh=-1; empty.nodes[1].name="tree"; Require(MarkWindNodes(empty)==0,"a veg_ node without a mesh is skipped");
}
void AnimationChecks()
{
    Animation animation; animation.source=Garden(); MarkWindNodes(animation.source); animation.windStrength=1.5f;
    const auto refs=pwgltf::Primitives(animation.source,1);
    size_t vertices=0; for(auto r:refs) vertices+=animation.source.meshes[size_t(animation.source.nodes[r.node].mesh)].primitives[r.primitive].indices.size();
    std::vector<TriVertex> output(vertices); animation.Start(output.data());
    // Expected values: the rest vertices of the dynamic primitives in source order, expanded through the index buffer.
    auto expected=[&](unsigned frame,bool previous) {
        std::vector<Vec3> list;
        for(auto r:refs) {
            const auto &p=animation.source.meshes[size_t(animation.source.nodes[r.node].mesh)].primitives[r.primitive];
            const auto weights=WindWeights(animation.source,r.node,r.primitive);
            for(auto i:p.indices) {
                const Vec3 rest=pwgltf::ToBench(p.positions[i]);
                list.push_back(rest+WindOffset(previous ? (frame>0 ? frame-1 : 0) : frame,rest,weights[i],1.5f));
            }
        }
        return list;
    };
    auto check=[&](unsigned frame,const char *message) {
        const auto now=expected(frame,false), before=expected(frame,true);
        Require(now.size()==output.size(),message);
        for(size_t i=0;i<output.size();++i) Require(SameVec(output[i].pos,now[i]) && SameVec(output[i].prevPos,before[i]),message);
    };
    Require(animation.Evaluate(0,0,0) && animation.geometryChanged,"first evaluation writes the geometry"); check(0,"frame 0: wind positions, no previous motion");
    // Frames advance with the clock stopped: the characters' time never changes, the wind still moves and refits.
    for(unsigned frame:{1u,2u,3u,50u}) {
        Require(animation.Evaluate(0,0,frame) && !animation.geometryChanged && animation.windChanged,"wind frame without animation time still evaluates and refits");
        check(frame,"wind positions at frame and previous at frame - 1");
    }
    Require(!animation.Evaluate(0,0,50) && !animation.windChanged,"the same frame again skips all work");
    // The trunk (weight 0) of a veg_ node stays at its rest position exactly
    const auto &p=animation.source.meshes[1].primitives[0];
    size_t first=0; for(auto r:refs) { if(r.node==1 && r.primitive==0) break; first+=animation.source.meshes[size_t(animation.source.nodes[r.node].mesh)].primitives[r.primitive].indices.size(); }
    for(size_t k=0;k<p.indices.size();++k) Require(SameVec(output[first+k].pos,pwgltf::ToBench(p.positions[p.indices[k]])),"trunk stays at rest");
}
}
void WindChecks() { OffsetChecks(); WeightChecks(); NodeChecks(); AnimationChecks(); std::puts("[test] wind checks passed"); }
