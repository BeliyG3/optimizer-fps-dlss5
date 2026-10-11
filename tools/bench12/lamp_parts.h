#pragma once
#include "lamps.h"
#include <array>
#include <vector>

// --lamp-tubes: the emitters of each lamp group split into connected parts (triangles that share a vertex position),
// each with its principal axes. Long flat parts are tubes, raster's one- or two-sided line lights; the others (screens,
// spots, square-section rods) stay compact, raster's point lights (raster_lights.h).
using LampTriangle=std::array<Vec3,3>;
struct LampPart {
    unsigned group=0;
    std::vector<unsigned> sources; // indices of the part's LampSources, ascending
    Vec3 a, b;      // the principal axis through the part's centre, from one end of its bounds to the other
    float radius=0; // half the mean of its two extents across the axis
    Vec3 normal;    // the emitting face's normal: the thin cross axis, pointing down (-y) when it is within 45 degrees of vertical
    bool twoSided=false; // the thin axis is not near vertical (a standing panel, a sign): it emits from both faces
    // The emitting faces as shares of the part's area: one face (length x wide extent) and one side (length x thin extent).
    // A Lambertian face of radiance Le and area A has the intensity Le A cos: the part's power times these shares.
    float faceShare=0, sideShare=0;
    bool tube=false;
};
// A part is a tube when its length is at least TubeMinAspect times its wider cross extent and that extent is at least
// TubeMinFace times the thin one (a dominant face; without one it stays compact). A thin axis within 45 degrees of
// vertical is a ceiling fixture's face: its housing hides the upper face, so the tube emits downward only.
constexpr float TubeMinAspect=4, TubeMinFace=2;
// Parts in the order of their first source. Connectivity: vertex positions equal to 0.1 mm, within one group only.
// The axis is the principal axis of the part's surface (its area-weighted second moment); the extents are the bounds
// of its corners along the three principal axes. `triangles` is parallel to `sources`.
std::vector<LampPart> FindLampParts(const std::vector<LampSource> &sources, const std::vector<LampTriangle> &triangles);
