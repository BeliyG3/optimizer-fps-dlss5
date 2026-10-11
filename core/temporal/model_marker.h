#pragma once

// TemporalModelMarker (2026.10.2): where full passes tell the model a pixel has no history (PSModelMotion's marker,
// PwTMarkerReason in shaders/temporal_residual.hlsli). The user setting picks 0 = no markers (the model gets the plain
// chain), 1 = classic (rule 0, as before 2026.10.2), 2 = narrow (rule 4: the colour test marks only where the depth
// mismatch reaches the tolerance, and a failing texel stays marked only when at least 8 of its 5x5 neighbourhood fail).
// The diagnostic keys still override it: DebugTemporalMarkerRule > 0 and DebugTemporalMarkerNeighbours > 0 replace the
// mapping, DebugTemporalNoModelMotion / DebugTemporalNoBackgroundModelMotion force the plain chain.
namespace ofps::core::temporal {

struct ModelMarker {
    bool sync = true;       // full passes hand the model ModelMotion's vectors (else the plain chain)
    bool background = true; // background kicks validate the model's vectors (else the copied chain)
    int rule = 0;           // FrameInputs::markerRule
    int neighbours = 0;     // FrameInputs::markerNeighbours (0 = 12)
};

constexpr int kNarrowMarkerRule = 4, kNarrowMarkerNeighbours = 8;

constexpr ModelMarker ResolveModelMarker(int setting, int debugRule, int debugNeighbours, bool debugNoModelMotion,
                                         bool debugNoBackgroundModelMotion)
{
    const bool on = setting != 0 || debugRule > 0;
    const bool narrow = setting == 2 && debugRule <= 0;
    ModelMarker m;
    m.sync = on && !debugNoModelMotion;
    m.background = on && !debugNoBackgroundModelMotion;
    m.rule = debugRule > 0 ? debugRule : (narrow ? kNarrowMarkerRule : 0);
    m.neighbours = debugNeighbours > 0 ? debugNeighbours : (narrow ? kNarrowMarkerNeighbours : 0);
    return m;
}

} // namespace ofps::core::temporal
