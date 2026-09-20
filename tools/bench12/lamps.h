#pragma once
#include "math.h"
#include <string>
#include <vector>

constexpr unsigned NoLamp=0xffffffffu;
struct LampGroup {
    std::string name;
    float intensity=1;
    Vec3 tint{1,1,1};
    unsigned triangles=0;
    float power=0;
};
struct EmissiveTriangle { unsigned triangle; float area, cdf, power; };
struct LampSource { unsigned triangle, group; float area; Vec3 emission; };
std::string LampName(std::string name);
unsigned FindLampGroup(std::vector<LampGroup> &groups, const std::string &name);
Vec3 LampFactor(const LampGroup &group);
float RebuildLampCdf(const std::vector<LampSource> &sources, std::vector<LampGroup> &groups,
                     std::vector<EmissiveTriangle> &lights);
