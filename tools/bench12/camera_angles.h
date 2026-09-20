#pragma once
#include "math.h"
inline Vec3 DirectionFromAngles(float azimuth, float elevation)
{
    constexpr float radians=3.14159265359f/180;
    float a=azimuth*radians,e=elevation*radians;
    return {std::sin(a)*std::cos(e),std::sin(e),std::cos(a)*std::cos(e)};
}
inline void DirectionAngles(Vec3 direction, float &azimuth, float &elevation)
{
    direction=Normalize(direction);
    constexpr float degrees=180/3.14159265359f;
    azimuth=std::atan2(direction.x,direction.z)*degrees;
    elevation=std::asin(std::clamp(direction.y,-1.0f,1.0f))*degrees;
}
