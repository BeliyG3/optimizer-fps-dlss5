#pragma once
// --render raster: culling of small instances (docs: INSTANCING.md, "Culling small objects"). An instance whose world
// bounding sphere (around its exact world bounds) has a radius below --cull-small-radius (0.75 m) is "small"; the hall,
// buildings, ground and trees are far larger and never take part. Each frame a small instance leaves every raster pass
// (camera passes, sun map or cascades, lamp spot maps) when its sphere lies wholly outside a cone of --cull-cone-deg (170)
// around the camera's view direction, or beyond --cull-small-distance (0 = off): objects just outside the view keep
// casting their shadows into it. The camera passes also drop small instances the Hi-Z of the frame's occluders hides
// (raster_occlusion.h). --no-small-culling turns both off.
#include "math.h"
#include <cstdint>
#include <vector>

struct InstanceBox;

// True when the sphere lies wholly outside the cone of full angle `coneDegrees` with apex `eye` along the unit `forward`,
// or wholly farther than `distance` from the eye (distance 0: no limit). A sphere around the eye is never outside.
inline bool SphereOutsideCone(Vec3 eye, Vec3 forward, float coneDegrees, float distance, Vec3 centre, float radius)
{
    const Vec3 d=centre-eye;
    const float length=std::sqrt(Dot(d,d));
    if(length<=radius) return false;
    if(distance>0 && length-radius>distance) return true;
    const float half=coneDegrees*0.5f*3.14159265f/180;
    const float angle=std::acos(std::clamp(Dot(d,forward)/length,-1.0f,1.0f)), spread=std::asin(std::min(1.0f,radius/length));
    return angle-spread>half; // the sphere's nearest direction to the axis is still outside the cone
}

struct SmallCull {
    float radius=0.75f, coneDegrees=170, distance=0;
    std::vector<uint8_t> smallMask; // per instance: 1 when small ("small" is a macro of the Windows headers)
    std::vector<Vec3> centres;
    std::vector<float> radii;
    std::vector<uint8_t> dropped; // per instance, this frame: 1 when outside the cone (or the distance)
    unsigned smallCount=0, droppedCount=0;
    // The small instances of the scene's world bounds.
    void Classify(const std::vector<InstanceBox> &boxes);
    // This frame's `dropped` for the camera at `eye` looking along `forward` (unit).
    void Update(Vec3 eye, Vec3 forward);
};
