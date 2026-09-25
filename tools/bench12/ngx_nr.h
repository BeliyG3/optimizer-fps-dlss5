#pragma once
#include "ngx_nr_bridge.h"
#include "ngx_nr_contract.h"
#include "ngx_nr_pad.h"
#include "ngx_nr_runtime.h"
#include "options.h"
#include "core_session.h"

// What the path tracer hands to feature 18 each frame. Depth and motion are render-size UAVs.
struct NrInputs {
    D3D12_RESOURCE_STATES colourState=D3D12_RESOURCE_STATE_UNORDERED_ACCESS; // of the configured colour
    ID3D12Resource *depth=nullptr, *motion=nullptr;
    float mvScaleX=1, mvScaleY=1;
    bool reset=false;
};

// DLSS Neural Rendering host (--nr native|upscale).
// native:  feature, colour and output at the colour's size (the DLSS SR/RR output, or the render
//          colour without an upscaler); guides at render size with their own sub-rects.
// upscale: feature at the render size, colour and guides at render size, output at output size.
//          The 310.8 runtime rejects this contract (see NGX_PARAMETERS.md); the evaluate result is
//          reported and the render colour is presented instead.
// With --nr-output-pad the output texture is larger than the frame and the frame-size region sits
// at a base offset (ngx_nr_pad.h). The colour passes through NrColourBridge on the way in and out.
class NeuralRendering {
public:
    NeuralRendering()=default;
    ~NeuralRendering();
    NeuralRendering(const NeuralRendering &)=delete;
    NeuralRendering &operator=(const NeuralRendering &)=delete;
    // (Re)creates the output and loads the runtime once; the feature is created by Prepare.
    void Configure(Device &device, const Options &options, ID3D12Resource *colour, unsigned renderWidth, unsigned renderHeight);
    // Creates the feature, but only once a frame has been presented after the runtime was loaded:
    // the Optimizer FPS add-on looks for nvngx_dlssnr.dll at present time and must see the create.
    void Prepare(Device &device, int frame);
    void Evaluate(Device &device, const NrInputs &inputs, int frame);
    // This frame's NR image (frame-size pixel-shader resource) when its evaluate succeeded, otherwise null.
    ID3D12Resource *Presented() const { return presented ? bridge.Presented() : nullptr; }
    bool Active() const { return mode!="off"; }
    void Shutdown(Device &device);
private:
    void Release(Device &device);
    NrRuntime runtime;
    NrCreateInfo create{};
    NrFrame currentFrame{};
    CoreSession coreSession{runtime, create, currentFrame};
    bool useCore=false;
    NgxParameterMap parameters;
    NrColourBridge bridge;
    NrOutputPad pad;
    NVSDK_NGX_Handle *feature=nullptr;
    ComPtr<ID3D12Resource> output;
    std::string mode="off";
    NrRect colourRect, guideRect, outputRect;
    bool depthInverted=false, deferCreate=false, pendingReset=true, presented=false;
    unsigned calls=0, succeeded=0, failed=0;
    NVSDK_NGX_Result lastResult=NVSDK_NGX_Result_Success;
};
