// GPU state of the bench scene: shaders and fixed-function state, the directional shadow map, the
// environment lighting and the procedural boxes. The glTF scene's own resources are in bench_gltf_scene.h.
#pragma once
#include "bench_d3d.h"
#include "bench_math.h"

struct BenchOptions;
struct BenchDevice;

inline constexpr UINT kShadowSize = 4096; // directional shadow map (4096^2, normal 0..1 depth, comparison sampling)

// The constant buffers of kShader3D (cbuffer Frame : b0, cbuffer Instances : b1).
struct FrameConstants {
    Mat4 vp, vpNoJitter, vpPrev, lightVp, invVp;
    float renderSize[4];
    float misc[4];
    float eyePos[4];
    float lightDir[4];
    float sunColor[4];
    float hdriParams[4];
    float env[4];
    float lab[4];
    float lightPos[24][4];
    float lightPower[24][4];
};
struct InstanceConstants {
    float pos[64][4];
    float prev[64][4];
    float size[64][4];
};

struct BenchScene {
    // ---- shaders and state ----
    ComPtr<ID3D11VertexShader> vsFlat, vsBox, vsGround, vsBlit, vsMesh, vsBoxShadow, vsGroundShadow, vsMeshShadow;
    ComPtr<ID3D11InputLayout> meshLayout;
    ComPtr<ID3D11PixelShader> psScene, psBlit, psAccum, psMeshShadow;
    ComPtr<ID3D11Buffer> cb, cbInst;
    ComPtr<ID3D11DepthStencilState> dss;
    ComPtr<ID3D11RasterizerState> rs;
    ComPtr<ID3D11BlendState> additive;
    ComPtr<ID3D11SamplerState> smp;
    // ---- directional shadow map ----
    ComPtr<ID3D11Texture2D> shadowTex; ComPtr<ID3D11DepthStencilView> shadowDsv; ComPtr<ID3D11ShaderResourceView> shadowSrv;
    ComPtr<ID3D11SamplerState> shadowSmp; ComPtr<ID3D11DepthStencilState> dssShadow;
    // ---- environment lighting from an equirect HDRI (--hdri): sky, ambient, sun ----
    ComPtr<ID3D11ShaderResourceView> hdriSrv; ComPtr<ID3D11SamplerState> hdriSampler;
    ComPtr<ID3D11PixelShader> psSky;
    bool hdriActive = false;
    Vec3 sunDir = Normalize(Vec3{0.4f, 0.8f, -0.35f}); // fallback: the old hardcoded direction
    float sunCol[3] = {0.85f, 0.85f, 0.85f};
    float hdriScale = 1.0f; // the HDRI is used at its raw radiance; --exposure is the only scale
    ComPtr<ID3D11DepthStencilState> dssNoDepth; // depth off for the sky pass
    // ---- the 3D scene: instances (boxes + the figure at the origin) ----
    InstanceConstants inst{};
    int instanceCount = 0;
    int movingA = 0, movingB = 0; // the last two boxes may move (--moving-boxes)
    // ---- the previous frame's unjittered view-projection (motion vectors) ----
    Mat4 vpPrevNoJitter = Identity();
    bool havePrev = false;
};

// Shaders, constant buffers, fixed-function state and (3D scene) the shadow map; false after printing the failure.
bool CreatePipeline(const BenchOptions &o, BenchDevice &d, BenchScene &s);
// The HDRI (sky, ambient, sun direction and colour, --hdri-yaw) and the --sun-dir / --sun-strength overrides.
void CreateLighting(BenchOptions &o, BenchDevice &d, BenchScene &s);
// The figure and the rings of boxes (deterministic), PW_BENCH_LIST_BOXES prints them.
void BuildBoxes(BenchScene &s);
// --moving-boxes: the two last boxes bob and circle; their previous positions carry the object motion vectors.
void AnimateBoxes(BenchScene &s, int frame);
