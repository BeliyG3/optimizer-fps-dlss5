#pragma once
#include "device.h"
#include "local_exposure.h"
#include <cmath>
#include <algorithm>

// Auto exposure of one frame (shaders/display.hlsl Adapt, display_math.hlsli): seconds to advance, the speeds (1/s)
// towards a brighter scene (the scale falls) and towards a darker one, the compensation (2^ev on the metered target) and
// the histogram's centre weighting (0 flat, 1: the corners count 1/16).
// ue: --exposure-metering ue (MeterBandAverage) within [minScale, maxScale]; local: --local-exposure's grid is built.
struct ExposureControl {
    bool automatic=false; float seconds=1.0f/60, speedUp=3, speedDown=1.5f, ev=0, centre=0;
    bool ue=false, local=false; float minScale=0, maxScale=3.0e38f;
    float exposure=1, curveLow=3, curveHigh=6, curveBias=0; // ue: --exposure (the scene EV100) and --exposure-curve
    bool replay=false; float replayScale=1; // --exposure-replay: Adapt writes replayScale instead (exposure_track.h)
};
// UE's Min/Max EV100 as bounds of the auto exposure's scale. UE's exposure for an EV100 is 1 / (1.2 * 2^EV100); the bench's
// total exposure is --exposure times the scale, so a bound is that over --exposure. The lower EV100 bounds the brighter
// end (a dark interior is lifted at most to it), the upper one the darker end.
inline void MeterScaleBounds(float minEv100, float maxEv100, float exposure, float &minScale, float &maxScale)
{
    const float e=exposure>1e-30f ? exposure : 1e-30f;
    maxScale=1.0f/(1.2f*std::exp2(minEv100))/e; minScale=1.0f/(1.2f*std::exp2(maxEv100))/e;
}
// The seconds the auto exposure advances: the real frame time interactively (at most 0.1 s, so a hitch does not jump it),
// a fixed 1/60 in scripted runs so dumps stay deterministic.
inline float ExposureSeconds(bool interactive, double elapsed)
{
    return interactive ? float(std::clamp(elapsed,0.0,0.1)) : 1.0f/60;
}
// The auto exposure of the last submitted frame (read back): the scale applied, the average-based and highlight-based
// targets (the scale moves towards the smaller one); valid once a frame has been metered.
struct ExposureReading { float scale=1, average=1, highlight=1; bool valid=false; };
// The firefly clamp the renderers apply (frame constant `firefly`, a luminance of the colour they write, which is the
// radiance times --exposure). --firefly is meant after all of the exposure: under auto exposure the display scales the
// frame again by its adapted scale, so the limit is --firefly divided by the last frame's scale. Before the first reading
// nothing is clamped. Without auto exposure it is --firefly as before. (Up to 2026-10-07 it was --firefly alone: with
// --exposure 1.45 and the garden's sun, every sunlit surface was clamped to the same luminance.)
inline float FireflyLimit(float firefly, bool autoExposure, const ExposureReading &last)
{
    if(!autoExposure) return firefly;
    return last.valid && last.scale>0 ? firefly/last.scale : 3.0e38f;
}

// Post-NGX linear pyramid, log-luminance reduction, metering histogram, presentation-only exposure and --local-exposure's
// grid. The present pass reads presentDescriptors +0..+2 (bloom), +3 (exposure), +4, +5 (local exposure).
class Display {
public:
    void Initialize(Device &device, ID3D12Resource *source, unsigned presentDescriptors);
    void Prepare(Device &device, ID3D12Resource *source, const ExposureControl &exposure);
    ExposureReading LastExposure() const; // after the frame's Submit
private:
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> downsample, histogram, adapt;
    std::vector<ComPtr<ID3D12Resource>> levels;
    ComPtr<ID3D12Resource> exposure, meter, meterReadback;
    unsigned descriptors=0, exposureUav=0, meterUav=0, meterLevel=0, sourceWidth=0, sourceHeight=0;
    LocalExposure local;
    bool initialized=false, metered=false; // metered: a frame's meter has been read back (kept across a resize)
};
