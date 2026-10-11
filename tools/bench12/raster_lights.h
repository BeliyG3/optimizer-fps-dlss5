#pragma once
#include "lamp_parts.h"
#include "lamps.h"
#include "math.h"
#include <cmath>
#include <vector>

// One point light per lamp group, in the layout the raster shader reads (32 bytes, HLSL-aligned).
struct RasterLight { Vec3 position; float padding0=0; Vec3 power; float padding1=0; };
static_assert(sizeof(RasterLight)==32, "RasterLight must match the HLSL structure");

// One light per group that has emitters: position is the emitter centroids weighted by area * luminance(emission);
// power is sum(area * emission) times LampFactor(group), in the units the path tracer uses.
// `centroids` is parallel to `sources`. Groups without emitters or with zero weight produce no light. `lightGroups`, when
// given, receives each light's group (--lamp-shadows shadows a light with its group's map).
std::vector<RasterLight> BuildRasterLights(const std::vector<LampSource> &sources, const std::vector<Vec3> &centroids,
                                           const std::vector<LampGroup> &groups, std::vector<unsigned> *lightGroups=nullptr);

// --lamp-tubes: one line light per tube (lamp_parts.h), 80 bytes, HLSL-aligned (shaders/raster_tubes.hlsli). Power is
// sum(area * emission) of the tube's triangles times LampFactor(group), the units of RasterLight::power. The tube is a
// box of Lambertian faces of the tube's radiance: toward a direction at angle theta to `normal` and phi to the side axis
// (`side` = unit(segment x normal)) its intensity is power * (faceShare * max(cos theta, 0) + sideShare * |cos phi|), |cos theta| when
// twoSided. A one-sided tube (a ceiling fixture) lights nothing above its face's plane: the housing hides its upper face
// and its sides. `range`: TubeRange.
struct RasterLine {
    Vec3 a; float radius=0; Vec3 b; float twoSided=0; Vec3 power; float range=0; Vec3 normal; float faceShare=0;
    float sideShare=0; Vec3 side;
};
static_assert(sizeof(RasterLine)==80, "RasterLine must match the HLSL structure");
// The range of a line light: where its irradiance falls to 0.5 % of its value at 1 m (inverse square: sqrt(1 / 0.005)),
// measured from the segment. TubeRangeWindow fades the light out from 0.75 range to range; beyond it the shader skips it.
constexpr float TubeRangeFraction=0.005f;
const float TubeRange=1/std::sqrt(TubeRangeFraction);
// The fade at distance^2 from the segment: 1 within 0.75 range, a smoothstep to 0 at the range. Mirror of the shader's.
float TubeRangeWindow(float distance2, float range);
// pointGroups and lineGroups: the group of each point and line, in their order.
struct RasterLampLights { std::vector<RasterLight> points; std::vector<RasterLine> lines; std::vector<unsigned> pointGroups, lineGroups; };
// The tube parts become lines; each group's compact parts become its point light as BuildRasterLights builds it from
// those emitters alone (a group without tubes keeps its light unchanged). Lines plus points carry every group's power.
RasterLampLights BuildRasterLampTubes(const std::vector<LampSource> &sources, const std::vector<Vec3> &centroids,
                                      const std::vector<LampGroup> &groups, const std::vector<LampPart> &parts);
// The irradiance at p (normal n) from a line light of total intensity 1 spread evenly from a to b whose intensity toward p
// is cos(the angle to m) (|cos| when twoSided; m perpendicular to the segment): the exact integral of
// max(m.(p-x), 0) max(n.(x-p), 0) / r^4 along the segment, clipped to the horizon of n; the distance to the axis held at
// `radius`. Far below it is the point light's n.l / d^2. CPU mirror of LineIrradiance in shaders/raster_tubes.hlsli.
float LineIrradiance(Vec3 p, Vec3 n, Vec3 a, Vec3 b, Vec3 m, bool twoSided, float radius);
// The irradiance at p (normal n) from `line` per unit of its power, before the range fade: its face and its sides (the
// rule above) through LineIrradiance. The shader shares one integral between them (c is constant along the segment).
float TubeIrradiance(Vec3 p, Vec3 n, const RasterLine &line);
