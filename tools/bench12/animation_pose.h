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
    // pwgltf::Scene::WorldsAt evaluates every node's channels by scanning all channels (O(nodes x channels): ~6 ms a frame with
    // the garden's people). The pose indexes the channels per node once and keeps the unanimated nodes' local matrices; the
    // result is the same matrices, computed the same way.
    std::vector<std::vector<size_t>> nodeChannels; // per node: its translation/rotation/scale channels, in scene order
    std::vector<std::vector<size_t>> weightChannels; // per node: its morph weight channels, in scene order
    std::vector<Mat4> restLocal; // per node without TRS channels: its local matrix
    void Initialize(const pwgltf::Scene &scene,const std::vector<pwgltf::PrimitiveRef> &refs);
    void Evaluate(const pwgltf::Scene &scene,float time);
private:
    void Worlds(const pwgltf::Scene &scene,float time);
    void Weights(const pwgltf::Scene &scene,size_t node,float time,std::vector<float> &out) const;
};
