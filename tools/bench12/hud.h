#pragma once
#include "device.h"
#include "hud_layout.h"

// --hud, both render modes: a game-like HUD alpha-blended into the render colour (outputs[0]) after the render and
// before NGX/NR, so the upscaler and NR see it as they see a game's HUD. Static panels, text and icons; the third
// label blinks every 30 frames.
class Hud {
public:
    // Root signature and PSO. Record calls it on first use, so runs without --hud never load the HUD shader.
    void Create(Device &device);
    // colour: an RGBA16F render-size target in UNORDERED_ACCESS (outputs[0], or outputs[8] when NGX is fed the
    // accumulated image); colourUav: its UAV descriptor in the shader-visible heap. It stays in that state.
    void Record(Device &device, ID3D12Resource *colour, unsigned colourUav, unsigned width, unsigned height, int frame);
private:
    struct Constants { // shaders/hud.hlsl, cbuffer HudFrame (b0), set as root constants
        HudLayout layout;
        float scale; unsigned labelVisible, width, height;
    };
    static_assert(sizeof(Constants)==112);
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pso;
};
