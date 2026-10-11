#pragma once
// GPU skinning (the default; --cpu-skinning keeps Animation's CPU workers): the CPU evaluates the poses only and uploads each
// dynamic node's palette (current and previous frame) and morph weights; a compute pass (shaders/skin.hlsl) deforms the shared
// source vertices (skin_layout.h) straight into the dynamic range of the scene vertex buffer, in the layout the CPU path writes
// (position, previous position, normal; UVs once at load), so the raster passes, the dynamic BLAS refit, the path tracer and
// the probes read it unchanged. Docs: INSTANCING.md, "GPU skinning".
#include "device.h"
#include "skin_layout.h"

class GpuSkinning {
public:
    // `dynamicVertices`: the dynamic range of the scene vertices before their upload (its UVs are written here);
    // `dynamicBase`: its first vertex index in the scene vertex buffer.
    GpuSkinning(Device &device, const pwgltf::Scene &source, const std::vector<unsigned> &order, TriVertex *dynamicVertices,
                unsigned dynamicBase, float windStrength);
    // Animation::Evaluate's contract: false when nothing moved since the last call (no pass needed); else the palettes and
    // weights are uploaded for Record. geometryChanged: the animation time moved; windChanged: the wind frame moved.
    // `windBefore`: the wind frame of the previous positions (windFrame - 1, or windFrame - K for --motion-span K).
    bool Evaluate(const pwgltf::Scene &source, float time, float previousTime, unsigned windFrame, unsigned windBefore);
    // The compute pass into `vertices` (NON_PIXEL_SHADER_RESOURCE on entry and on return; a transition orders it before the
    // dynamic BLAS refit and the renderers' reads). Sets its own compute root and pipeline.
    void Record(Device &device, ID3D12Resource *vertices) const;
    double cpuMs=0;
    bool geometryChanged=false, windChanged=false;
    const SkinLayout &Layout() const { return layout; }
private:
    SkinLayout layout;
    AnimationPose currentPose, previousPose;
    std::vector<Affine3x4> lastPalette; // the last evaluated current palette, the previous one when time steps on
    std::vector<float> lastWeights;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pso;
    ComPtr<ID3D12Resource> sources, items, vertexItem, offsets, outputs, targets, wind, frame;
    Affine3x4 *mappedPalette=nullptr; // [current entries][previous entries], then the weights
    float *mappedWeights=nullptr;
    unsigned dynamicBase=0;
    float windStrength=1, now=0, before=0;
    unsigned windNow=0, windBefore=0;
    bool evaluated=false;
};
