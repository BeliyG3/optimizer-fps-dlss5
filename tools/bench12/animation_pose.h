#pragma once
#include "animation_source.h"
#include "animation_affine.h"

// Prepared before releasing workers; all frame data is shared read-only.
struct AnimationPose {
    struct Node { std::vector<float> weights; std::vector<Affine3x4> joints; };
    std::vector<Mat4> worlds,local;
    std::vector<int> stack;
    std::vector<Node> nodes;
    std::vector<std::vector<Mat4>> skins;
    std::vector<size_t> activeNodes,activeSkins;
    void Initialize(const pwgltf::Scene &scene,const std::vector<pwgltf::PrimitiveRef> &refs);
    void Evaluate(const pwgltf::Scene &scene,float time);
};
