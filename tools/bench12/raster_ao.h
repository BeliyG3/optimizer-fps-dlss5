#pragma once
#include "device.h"

// --render raster --ao: screen-space ambient occlusion. A depth/normal prepass draws the scene pass's depth buffer and
// an octahedral geometric normal (shaders/raster_prepass.hlsl), compute passes turn them into the occlusion
// (shaders/raster_ao.hlsl, raster_ao_temporal.hlsl), and the scene pass, its depth test EQUAL to the prepass and depth
// writes off, reads it as aoTexture (t4 space2). Raster creates and drives this (raster_ao.cpp, raster_pipeline.cpp);
// null without --ao.
// clean: CSAo (8 slice directions) -> CSAoBlur -> `out`, which the scene pass reads.
// game: the prepass also writes motion; CSAo (2 directions, per-frame IGN noise) -> CSAoBlur (2x2) -> `out` ->
// CSAoTemporal, which blends `out` with the previous frame's history into this frame's history; the scene pass reads that.
struct RasterAo {
    // The scene pass's root signature of this run (graphicsRoot's, the baked or the CSM root's parameters) plus the
    // AO table at sceneParameter. The prepass uses it too, so the bindings set once serve both passes.
    ComPtr<ID3D12RootSignature> sceneRoot, computeRoot;
    ComPtr<ID3D12PipelineState> prepassPso[2], scenePso[2], aoPso, blurPso, temporalPso; // [0] standard depth, [1] reverse
    ComPtr<ID3D12DescriptorHeap> rtvHeap; // the normal target's RTV, then the motion target's (game)
    unsigned rtvStep=0, sceneParameter=0;
    unsigned srvBase=0; // normal, raw AO, AO, motion, history[0], history[1] (t3..t4 of CSAoTemporal: AO, motion)
    unsigned uavBase=0; // raw AO, AO, history[0], history[1]
    float radius=1, strength=1, alpha=0.1f; // alpha: --ao-alpha, game's history blend
    bool game=false;
    // Render-size targets, created by the first raster frame after UseAo or Release. Resting states between frames:
    // normal and motion NON_PIXEL_SHADER_RESOURCE, raw UNORDERED_ACCESS, out PIXEL_SHADER_RESOURCE (clean) or
    // NON_PIXEL_SHADER_RESOURCE (game), history both shader-read states.
    ComPtr<ID3D12Resource> normal, raw, out; // RG16_SNORM, R8_UNORM, R8_UNORM
    ComPtr<ID3D12Resource> motion, history[2]; // game only: RG16_FLOAT; R16G16_FLOAT (AO, view depth)
    unsigned width=0, height=0;
    // game: this frame writes history[current] and reads history[current^1] when historyValid (BeginFrame).
    unsigned current=0;
    bool recreated=true, historyValid=false;
    int previousFrame=-1;

    // Raster::Resize: the old targets have retired (the previous frame's fence); the next frame recreates them.
    void Release();
    void EnsureTargets(Device &device, unsigned width, unsigned height);
    // Once per AO frame, after EnsureTargets: picks this frame's history texture and whether the other one is usable.
    void BeginFrame(unsigned frameIndex);
    D3D12_CPU_DESCRIPTOR_HANDLE NormalRtv() const { return rtvHeap->GetCPUDescriptorHandleForHeapStart(); }
    // The scene pass's aoTexture: `out` (clean) or this frame's history (game).
    unsigned SceneSrv() const { return game ? srvBase+4+current : srvBase+2; }
    // After the prepass: `depth` (the SRV of the scene depth buffer), `normal` (and `motion`) readable by compute.
    // Computes the occlusion and leaves the scene pass's aoTexture in PIXEL_SHADER_RESOURCE. reverse: the depth
    // convention; frameIndex: game's noise frame.
    void Record(Device &device, D3D12_GPU_VIRTUAL_ADDRESS frameConstants, D3D12_GPU_DESCRIPTOR_HANDLE depth, bool reverse,
                unsigned frameIndex);
};
