#pragma once
#include "accel.h"
#include "camera.h"
#include "ngx.h"
#include "ngx_nr.h"
#include "motion_pack.h"
#include "display.h"
#include <array>

// Each row after the matrices is one HLSL constant-register (16 bytes).
struct FrameConstants {
    Mat4 currentVP, previousVP;
    Vec3 eye; float tanHalf;
    Vec3 right; float aspect;
    Vec3 up; float exposure;
    Vec3 forward; float albedo;
    Vec3 sunTravel; float sunStrength;
    unsigned width, height, frame, spp;
    unsigned bounces, opaqueTriangles, lightCount, accumulation;
    float jitterX, jitterY, firefly, sunCos;
    float lightPower; unsigned reverse, view, lightCandidates;
    float hazeDensity, hazeG, bloom; unsigned neutralTonemap;
    int pickX=-1, pickY=-1; unsigned accumulationLimit=UINT_MAX, padding=0;
    unsigned dynamicBase=0, dynamicOpaque=0, motionNdc=0, animationPadding=0;
};
static_assert(sizeof(FrameConstants)==320);

class Pathtrace {
public:
    explicit Pathtrace(Device &device);
    void Shutdown(Device &device) { nr.Shutdown(device); ngx.Shutdown(device); }
    void Render(Device &device, const Scene &scene, Accel &accel, Options &options, int frame, bool dump,
                const CameraState *camera=nullptr, void (*overlay)(Device &)=nullptr, float frameDelta=1.0f/60);
    void Reset() { accumulated=0; resetHistory=true; }
    void RequestPick(int x, int y) { pickX=x; pickY=y; }
    bool ReadPick(unsigned &triangle, unsigned &group);
    unsigned Accumulated() const { return accumulated; }
    const std::string &Error() const { return error; }
    unsigned Width() const { return width; }
    unsigned Height() const { return height; }
private:
    void CreatePipelines(Device &device);
    void Configure(Device &device, Options &options);
    void ConfigureNeuralRendering(Device &device, Options &options);
    // Chooses the presented image after NGX/NR ran, re-creating the display pyramid on a change.
    void BindDisplay(Device &device, const Options &options);
    ID3D12Resource *DisplaySource(const Options &options) const;
    ComPtr<ID3D12Resource> pickBuffer, pickReadback;
    int pickX=-1, pickY=-1;
    bool pickPending=false, resetHistory=false;
    unsigned outputWidth=0, outputHeight=0;
    std::string configuredUpscaler, configuredNr, configuredMotion, displayKey, error;
    bool configuredReverse=false;
    ComPtr<ID3D12RootSignature> traceRoot, presentRoot;
    ComPtr<ID3D12PipelineState> tracePso, presentPso;
    ComPtr<ID3D12Resource> constants;
    std::array<ComPtr<ID3D12Resource>,9> outputs;
    Display display;
    Ngx ngx;
    NeuralRendering nr;
    MotionPack motionPack;
    unsigned uavBase=0, srvBase=0, width=0, height=0, accumulated=0;
    CameraState previous{};
    Mat4 previousVP{};
    bool initialized=false;
};
