#pragma once
#include "lamp_parts.h"
#include "lamps.h"
#include "math.h"
#include <vector>

// --lamp-shadows (RASTER.md "Lamp shadows"): every lamp group gets a spot shadow map, as a stationary light in Unreal.
// This header is the CPU side shared with the checks (tests_lamp_shadows.cpp): the frustum of each group and the
// visibility weights the shader mirrors (shaders/raster_lamp_shadow.hlsli). raster_lamp_shadow_passes.cpp draws them.
constexpr unsigned LampShadowResolution=1024; // texels per side of each group's map
constexpr unsigned MaxLampShadows=16;         // maps (array slices); groups beyond the first 16 stay unshadowed

// One lamp group's spot shadow map: a perspective frustum looking straight down from `apex`.
// - The lit area is the group's emitter footprint plus a 60 degree spread from the emitters' height above the floor:
//   radius litRadius = e + sqrt(3) h on the floor, e the footprint's half diagonal, h the power-weighted centroid's
//   height above the floor.
// - The apex is above the footprint's centre, e above the centroid (a lone lamp: at the lamp; a field of tubes: high
//   enough that the shadows fall nearly straight down, as under the field's average light). The field of view covers the
//   lit area: 2 atan(litRadius / (apex height above the floor)), which is 120 degrees for a lone lamp and tends to 90
//   for a wide field (clamped to 90..120).
// - The near plane is 5 cm below the lowest emitter, so the lamps, their housings and the ceiling above never cast;
//   the far plane is just below the floor.
struct LampShadowFrustum {
    unsigned group=0;
    Vec3 apex, centre; // centre: the power-weighted centroid of the emitters (the group's point light), the weights' origin
    float fov=0, nearPlane=0, farPlane=0, litRadius=0; // fov: the full angle, radians
    float texelScale=0; // one texel's width per metre of view depth: 2 tan(fov / 2) / LampShadowResolution
    Mat4 viewProjection; // clip x, y in [-1, 1] over the lit area at the floor; depth 0 at the near plane, 1 at the far one
};
constexpr Vec3 LampShadowAxis{0,-1,0}; // every map looks straight down

// One frustum per group that has emitters (sum of area x luminance(emission) > 0), in group order, at most
// MaxLampShadows. `triangles` is parallel to `sources` (Scene::lampTriangles); floorY is the scene's lowest y.
std::vector<LampShadowFrustum> FitLampShadows(const std::vector<LampSource> &sources, const std::vector<LampTriangle> &triangles,
                                              size_t groupCount, float floorY);
// The slice of each group's map (an index into the frusta), or NoLampShadow for a group without one.
constexpr unsigned NoLampShadow=0xffffffffu;
std::vector<unsigned> LampShadowSlices(const std::vector<LampShadowFrustum> &frusta, size_t groupCount);

// The baked mode's lamp visibility at a point (mirror of LampVisibility in shaders/raster_lamp_shadow.hlsli): the average
// of each group's shadow test `visibility[i]` (1 lit, 0 shadowed) weighted by the group's luminance power times its point
// light's falloff at p (LampFalloff: 1/4 over the distance squared, held at 0.25 m). 1 when no group weighs anything.
float LampShadowWeight(const LampShadowFrustum &frustum, float powerLuminance, Vec3 p);
float LampVisibility(const std::vector<LampShadowFrustum> &frusta, const std::vector<float> &powerLuminance,
                     const std::vector<float> &visibility, Vec3 p);
