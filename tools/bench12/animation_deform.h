#pragma once
#include "animation_source.h"
#include "animation_affine.h"

inline void DeformVertex(const pwgltf::Primitive &prim,size_t v,const std::vector<float> &weights,
    const std::vector<Affine3x4> &joints,const Mat4 &world,Vec3 &position,Vec3 &normal)
{
    Vec3 p=prim.positions[v], n=v<prim.normals.size() ? prim.normals[v] : Vec3{0,1,0};
    for(size_t k=0;k<prim.targets.size() && k<weights.size();++k) {
        if(weights[k]==0 || v>=prim.targets[k].size()) continue;
        p=p+prim.targets[k][v]*weights[k];
    }
    if(!prim.joints.empty() && !joints.empty()) {
        Affine3x4 blend; float total=0;
        for(size_t k=0;k<4;++k) {
            const float w=prim.skinWeights[v*4+k]; const size_t j=prim.joints[v*4+k];
            if(w==0 || j>=joints.size()) continue;
            blend.Add(joints[j],w); total+=w;
        }
        if(total>0) { p=blend.Point(p)*(1/total); n=Normalize(blend.Direction(n)); }
    }
    position=pwgltf::ToBench(pwgltf::TransformPoint(world,p));
    normal=Normalize(pwgltf::ToBench(pwgltf::TransformDir(world,n)));
}
