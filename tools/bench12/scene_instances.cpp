#include "scene_instances.h"
#include <algorithm>
#include <map>
#include <numeric>
#include <stdexcept>
#include <tuple>

namespace {
bool InstancableMesh(const pwgltf::Mesh &mesh)
{
    if(mesh.primitives.empty()) return false;
    for(const auto &p:mesh.primitives)
        if(!p.targets.empty() || p.emission[0]+p.emission[1]+p.emission[2]>0.0f) return false;
    return true;
}
// The node's mesh in object space (bench coordinates): BuildVerticesSubset's vertices without the world transform, and
// each triangle's roughness and foliage class (CollectFlatTriangles' traversal).
void ObjectVertices(const pwgltf::Scene &s, size_t node, std::vector<pwgltf::Vertex> &out, std::vector<float> &roughness,
                    std::vector<VegClass> &vegClass)
{
    const auto &n=s.nodes[node]; const auto &mesh=s.meshes[size_t(n.mesh)];
    std::vector<Vec3> dp,dn; const std::vector<float> weights; const std::vector<Mat4> joints;
    for(const auto &prim:mesh.primitives) {
        size_t valid=0; for(auto index:prim.indices) if(index<prim.positions.size()) ++valid;
        if(valid%3) throw std::runtime_error("Invalid glTF primitive triangle indices");
        pwgltf::DeformPrimitive(prim,weights,joints,dp,dn);
        for(uint32_t index:prim.indices) {
            if(index>=prim.positions.size()) continue;
            out.push_back(pwgltf::PrimitiveVertex(n,mesh,prim,index,false,pwgltf::ToBench(dp[index]),
                                                  Normalize(pwgltf::ToBench(dn[index])),dp[index].y));
        }
        roughness.insert(roughness.end(),valid/3,std::clamp(prim.roughness,0.0f,1.0f));
        vegClass.insert(vegClass.end(),valid/3,VegClassOf(n.name,prim.materialName));
    }
}
void Grow(InstanceBox &box, Vec3 p)
{
    box.lo={std::min(box.lo.x,p.x),std::min(box.lo.y,p.y),std::min(box.lo.z,p.z)};
    box.hi={std::max(box.hi.x,p.x),std::max(box.hi.y,p.y),std::max(box.hi.z,p.z)};
}
}

InstancePlan PlanInstances(const pwgltf::Scene &s, bool enabled)
{
    InstancePlan plan; plan.instanced.assign(s.nodes.size(),false);
    if(!enabled) return plan;
    std::vector<int> usable(s.meshes.size(),-1); // -1 unknown, 0 no, 1 yes
    std::map<std::tuple<int,bool,int>,size_t> keys; std::vector<InstanceGroup> candidates;
    for(size_t i=0;i<s.nodes.size();++i) {
        const auto &n=s.nodes[i];
        if(n.mesh<0 || size_t(n.mesh)>=s.meshes.size() || n.skin>=0 || pwgltf::DynamicNode(s,i)) continue;
        int &ok=usable[size_t(n.mesh)];
        if(ok<0) ok=InstancableMesh(s.meshes[size_t(n.mesh)]) ? 1 : 0;
        if(!ok) continue;
        // The palette colour reaches the vertices only through primitives without their own colour.
        const auto &primitives=s.meshes[size_t(n.mesh)].primitives;
        const bool palette=std::any_of(primitives.begin(),primitives.end(),[](const pwgltf::Primitive &p) { return !p.hasColour; });
        const auto key=std::make_tuple(n.mesh,n.isGround,palette ? n.pwColour : -1);
        auto found=keys.find(key);
        if(found==keys.end()) { found=keys.emplace(key,candidates.size()).first; candidates.push_back({n.mesh,{}}); }
        candidates[found->second].nodes.push_back(i);
    }
    for(auto &group:candidates) {
        if(group.nodes.size()<2) continue;
        for(size_t node:group.nodes) plan.instanced[node]=true;
        plan.groups.push_back(std::move(group));
    }
    return plan;
}

void BenchRows(const Mat4 &world, float rows[3][4])
{
    // bench = C * glTF with C = diag(1, 1, -1): the bench rows are C * world * C, the translation C * world's.
    const float c[3]={1,1,-1};
    for(int i=0;i<3;++i) {
        for(int j=0;j<3;++j) rows[i][j]=c[i]*world.m[i][j]*c[j];
        rows[i][3]=c[i]*world.m[i][3];
    }
}

InstancedGeometry BuildInstancedGeometry(const pwgltf::Scene &s, const InstancePlan &plan, unsigned firstTriangle)
{
    InstancedGeometry g;
    if(plan.groups.empty()) return g;
    std::vector<Mat4> worlds,local; std::vector<int> stack;
    s.WorldsAt(0,worlds,local,stack); // static nodes: any time gives the same matrices
    bool any=false;
    for(const auto &group:plan.groups) {
        // ponytail: the first node names the class of the whole group; split groups by class if instances of one mesh differ.
        std::vector<pwgltf::Vertex> raw; std::vector<float> roughness; std::vector<VegClass> vegClass;
        ObjectVertices(s,group.nodes.front(),raw,roughness,vegClass);
        const unsigned triangles=unsigned(raw.size()/3);
        if(!triangles) continue;
        std::vector<unsigned> order(triangles); std::iota(order.begin(),order.end(),0u);
        const auto split=std::stable_partition(order.begin(),order.end(),[&](unsigned t) { return !raw[size_t(t)*3].alphaMask; });
        InstancedMesh mesh{firstTriangle,triangles,unsigned(split-order.begin()),unsigned(g.instances.size()),unsigned(group.nodes.size())};
        for(unsigned t:order) {
            for(unsigned k=0;k<3;++k) g.vertices.push_back(raw[size_t(t)*3+k]);
            g.roughness.push_back(roughness[t]); g.vegClass.push_back(vegClass[t]);
        }
        for(size_t node:group.nodes) {
            GpuInstance instance{};
            BenchRows(worlds[node],instance.world);
            std::copy(&instance.world[0][0],&instance.world[0][0]+12,&instance.previous[0][0]); // static: no motion
            instance.firstTriangle=mesh.firstTriangle; instance.opaqueTriangles=mesh.opaqueTriangles;
            // The world bounds as the flattened vertices had them: BuildVertices' ToBench(TransformPoint(world, p)).
            InstanceBox box{};
            for(size_t v=0;v<raw.size();++v) {
                const Vec3 p=pwgltf::ToBench(pwgltf::TransformPoint(worlds[node],pwgltf::ToBench(raw[v].pos)));
                if(!v) box={p,p}; else Grow(box,p);
            }
            if(!any) g.bounds=box; else { Grow(g.bounds,box.lo); Grow(g.bounds,box.hi); }
            any=true;
            g.instances.push_back(instance); g.boxes.push_back(box);
        }
        g.meshes.push_back(mesh);
        firstTriangle+=triangles;
    }
    return g;
}

FlatTriangles CollectFlatTriangles(const pwgltf::Scene &s, const InstancePlan &plan)
{
    FlatTriangles f; unsigned dynamicOffset=0;
    for(size_t node=0;node<s.nodes.size();++node) {
        const auto &n=s.nodes[node];
        if(n.mesh<0 || size_t(n.mesh)>=s.meshes.size() || plan.instanced[node]) continue;
        const bool moving=pwgltf::DynamicNode(s,node);
        for(const auto &p:s.meshes[size_t(n.mesh)].primitives) {
            size_t valid=0; for(auto index:p.indices) if(index<p.positions.size()) ++valid;
            if(valid%3) throw std::runtime_error("Invalid glTF primitive triangle indices");
            f.roughness.insert(f.roughness.end(),valid/3,std::clamp(p.roughness,0.0f,1.0f));
            f.materialNames.insert(f.materialNames.end(),valid/3,p.materialName);
            f.dynamic.insert(f.dynamic.end(),valid/3,moving); f.wind.insert(f.wind.end(),valid/3,n.wind);
            f.vegClass.insert(f.vegClass.end(),valid/3,VegClassOf(n.name,p.materialName));
            for(size_t k=0;k<valid/3;++k) f.dynamicIndex.push_back(moving ? dynamicOffset++ : 0);
        }
    }
    return f;
}
