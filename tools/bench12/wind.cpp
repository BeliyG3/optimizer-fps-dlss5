#include "wind.h"
#include <algorithm>
#include <cctype>
#include <numeric>

namespace {
std::string Lower(std::string text)
{
    for(char &c:text) c=char(std::tolower(static_cast<unsigned char>(c)));
    return text;
}
bool IsGrassNode(const pwgltf::Scene &scene,size_t node) { return Lower(scene.nodes[node].name).find("grass")!=std::string::npos; }
bool ValidMesh(const pwgltf::Scene &scene,size_t node) { return scene.nodes[node].mesh>=0 && size_t(scene.nodes[node].mesh)<scene.meshes.size(); }
Vec3 RestPosition(const Mat4 &world,Vec3 local) { return pwgltf::ToBench(pwgltf::TransformPoint(world,local)); }
struct Heights { float lo=0, hi=0; };
// Lowest and highest rest vertex of the node's mesh, in bench space.
Heights NodeHeights(const pwgltf::Scene &scene,size_t node)
{
    Heights h{1e30f,-1e30f};
    if(!ValidMesh(scene,node)) return {};
    const Mat4 world=scene.WorldAt(int(node),0);
    for(const auto &prim:scene.meshes[size_t(scene.nodes[node].mesh)].primitives) for(const Vec3 &p:prim.positions) {
        const float y=RestPosition(world,p).y; h.lo=std::min(h.lo,y); h.hi=std::max(h.hi,y);
    }
    return h.lo>h.hi ? Heights{} : h;
}
float Fraction(Heights h,float y) { return h.hi-h.lo>1e-6f ? std::clamp((y-h.lo)/(h.hi-h.lo),0.0f,1.0f) : 0.0f; }
float FromFraction(bool grass,float fraction) { return grass ? (fraction>0.5f ? 1.0f : 0.0f) : fraction*fraction; }
size_t Find(std::vector<size_t> &parent,size_t i)
{
    while(parent[i]!=i) { parent[i]=parent[parent[i]]; i=parent[i]; }
    return i;
}
// Grass: the top and the bottom of every quad, found as connected components of the vertex graph.
std::vector<float> GrassWeights(const pwgltf::Primitive &prim,const Mat4 &world)
{
    const size_t count=prim.positions.size();
    std::vector<size_t> parent(count); std::iota(parent.begin(),parent.end(),size_t(0));
    for(size_t k=0;k+2<prim.indices.size();k+=3) {
        const uint32_t a=prim.indices[k],b=prim.indices[k+1],c=prim.indices[k+2];
        if(a>=count || b>=count || c>=count) continue;
        parent[Find(parent,b)]=Find(parent,a); parent[Find(parent,c)]=Find(parent,a);
    }
    std::vector<Heights> range(count,Heights{1e30f,-1e30f}); std::vector<float> y(count);
    for(size_t v=0;v<count;++v) {
        y[v]=RestPosition(world,prim.positions[v]).y; Heights &h=range[Find(parent,v)];
        h.lo=std::min(h.lo,y[v]); h.hi=std::max(h.hi,y[v]);
    }
    std::vector<float> weights(count);
    for(size_t v=0;v<count;++v) weights[v]=FromFraction(true,Fraction(range[Find(parent,v)],y[v]));
    return weights;
}
}

bool IsWindNode(const pwgltf::Node &node) { return node.mesh>=0 && node.name.starts_with("veg_"); }
size_t MarkWindNodes(pwgltf::Scene &scene)
{
    size_t count=0;
    for(auto &node:scene.nodes) if(IsWindNode(node)) { node.wind=true; ++count; }
    return count;
}
bool IsTrunkMaterial(const std::string &material)
{
    const std::string name=Lower(material);
    if(name.find("trunk")!=std::string::npos || name.find("bark")!=std::string::npos) return true;
    const std::string tree="island_tree_";
    return name.size()>tree.size() && name.starts_with(tree) &&
        std::all_of(name.begin()+std::ptrdiff_t(tree.size()),name.end(),[](char c) { return std::isdigit(static_cast<unsigned char>(c))!=0; });
}
float WindWeight(const pwgltf::Scene &scene,size_t node,const Vec3 &restPos,const std::string &material)
{
    if(IsTrunkMaterial(material)) return 0;
    return FromFraction(IsGrassNode(scene,node),Fraction(NodeHeights(scene,node),restPos.y));
}
std::vector<float> WindWeights(const pwgltf::Scene &scene,size_t node,size_t primitive)
{
    if(!ValidMesh(scene,node)) return {};
    const auto &primitives=scene.meshes[size_t(scene.nodes[node].mesh)].primitives;
    if(primitive>=primitives.size()) return {};
    const auto &prim=primitives[primitive];
    if(IsTrunkMaterial(prim.materialName)) return std::vector<float>(prim.positions.size(),0.0f);
    const Mat4 world=scene.WorldAt(int(node),0);
    const bool grass=IsGrassNode(scene,node);
    if(grass) return GrassWeights(prim,world);
    const Heights h=NodeHeights(scene,node);
    std::vector<float> weights(prim.positions.size());
    for(size_t v=0;v<weights.size();++v) weights[v]=FromFraction(false,Fraction(h,RestPosition(world,prim.positions[v]).y));
    return weights;
}
Vec3 WindOffset(unsigned frame,Vec3 restPos,float weight,float strength)
{
    if(weight==0 || strength==0) return {};
    // Phases in double: at frame 10^6 a float phase has lost 0.004 rad.
    const double t=double(frame)/60.0, p=double(restPos.x)*kWindDirX+double(restPos.z)*kWindDirZ;
    const double sway=kWindAmplitude*std::sin(kWindOmega*t+kWindWave*p)+kWindFlutter*std::sin(2.7*kWindOmega*t+1.3*kWindWave*p);
    const float s=float(double(strength)*weight*sway);
    return {kWindDirX*s,0,kWindDirZ*s};
}
