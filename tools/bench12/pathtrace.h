#pragma once
#include "accel.h"
#include "camera.h"
#include "ngx.h"
#include "ngx_nr.h"
#include "motion_pack.h"
#include "display.h"
#include "exposure_track.h"
#include "frame_constants.h"
#include "raster.h"
#include "hud.h"
#include <array>

class Pathtrace {
public:
    explicit Pathtrace(Device &device);
    double dumpWriteSeconds=0; // the dumped frames' readback, encode and file writes (--startup-profile)
    void Shutdown(Device &device) { raster.ReportCulling(); nr.Shutdown(device); ngx.Shutdown(device); }
    // --raster-sky-ambient: the raster frame's ambient from the HDRI sky (Raster::UseSkyAmbient), once before UseLightmap.
    void UseSkyAmbient(Device &device, const Options &options) { raster.UseSkyAmbient(device,options); }
    // --probes: the raster frame's ambient from the traced probe grid (Raster::UseProbes), once after UseSkyAmbient.
    void UseProbes(Device &device, const ProbeGrid &grid) { raster.UseProbes(device,grid); }
    // --lamp-tubes: the raster frame's lamp tubes as line lights (Raster::UseLampTubes), once after UseProbes.
    void UseLampTubes(Device &device, const Scene &scene) { raster.UseLampTubes(device,scene); }
    // --lamp-shadows: the raster frame's lamp groups' spot shadow maps (Raster::UseLampShadows), once after UseLampTubes.
    void UseLampShadows(Device &device, const Scene &scene, const Options &options) { raster.UseLampShadows(device,scene,options); }
    // --sky-reflections: the raster frame's sky specular resources (Raster::UseSkyReflections), once in every run.
    void UseSkyReflections(Device &device, const Scene &scene) { raster.UseSkyReflections(device,scene); }
    // --fxaa: the raster resolve filtered with FXAA 3.11 (Raster::UseFxaa), once after Create.
    void UseFxaa(Device &device) { raster.UseFxaa(device); }
    // --lightmap: the raster frame's baked mode (Raster::UseLightmap), once before the first frame.
    void UseLightmap(Device &device, const Scene &scene, const Options &options) { raster.UseLightmap(device,scene,options); }
    // --csm: the raster frame's cascaded sun shadow (Raster::UseCsm), once after UseLightmap.
    void UseCsm(Device &device, const Options &options) { raster.UseCsm(device,options); }
    // --ao: the raster frame's prepass and ambient occlusion (Raster::UseAo), once after UseCsm.
    void UseAo(Device &device, const Options &options) { raster.UseAo(device,options); }
    // --exposure-record / --exposure-replay (exposure_track.h), once before the first frame; neither: nothing.
    void UseExposureTrack(const Options &options)
    {
        if(!options.exposureRecord.empty()) exposureTrack.Record(options.exposureRecord);
        if(!options.exposureReplay.empty()) exposureTrack.Replay(options.exposureReplay);
    }
    void Render(Device &device, const Scene &scene, Accel &accel, Options &options, int frame, bool dump,
                const CameraState *camera=nullptr, void (*overlay)(Device &)=nullptr, float frameDelta=1.0f/60,
                float exposureSeconds=1.0f/60); // exposureSeconds: ExposureSeconds (display.h)
    void Reset() { accumulated=0; resetHistory=true; }
    // --variants: the next frame starts the window again: feature 18 anew, history reset, previous view = frame - 1's.
    void Rewind(Device &device) { nr.Restart(device); Reset(); rewound=true; }
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
    std::string configuredUpscaler, configuredNr, configuredMotion, configuredRender, displayKey, error;
    bool configuredReverse=false;
    ComPtr<ID3D12RootSignature> traceRoot, presentRoot;
    ComPtr<ID3D12PipelineState> tracePso, presentPso;
    ComPtr<ID3D12Resource> constants;
    std::array<ComPtr<ID3D12Resource>,9> outputs;
    Display display;
    ExposureTrack exposureTrack;
    Ngx ngx;
    NeuralRendering nr;
    MotionPack motionPack;
    Raster raster; // --render raster, and the --motion none pass of both render modes
    Hud hud; // --hud, both render modes
    unsigned uavBase=0, srvBase=0, width=0, height=0, accumulated=0;
    CameraState previous{};
    Mat4 previousVP{};
    bool initialized=false, rewound=false;
};
