#pragma once
// The few HLSL types and intrinsics the shared headers (shaders/tonemap_ue.hlsli, shaders/local_exposure.hlsli,
// shaders/sky_reflection_math.hlsli) use, so the CPU checks run the shaders' own code.
#include <algorithm>
#include <cmath>

namespace hlsl {
using uint=unsigned;
struct float2 {
    float x=0, y=0;
    float2()=default;
    float2(float a, float b) : x(a), y(b) {}
};
inline float2 operator+(float2 a, float2 b) { return {a.x+b.x,a.y+b.y}; }
inline float2 operator/(float2 a, float s) { return {a.x/s,a.y/s}; }
struct float3 {
    float x=0, y=0, z=0;
    float3()=default;
    explicit float3(float v) : x(v), y(v), z(v) {}
    float3(float a, float b, float c) : x(a), y(b), z(c) {}
};
inline float3 operator+(float3 a, float3 b) { return {a.x+b.x,a.y+b.y,a.z+b.z}; }
inline float3 operator-(float3 a, float3 b) { return {a.x-b.x,a.y-b.y,a.z-b.z}; }
inline float3 operator*(float3 a, float s) { return {a.x*s,a.y*s,a.z*s}; }
inline float3 operator*(float3 a, float3 b) { return {a.x*b.x,a.y*b.y,a.z*b.z}; }
inline float3 operator*(float s, float3 a) { return a*s; }
inline float dot(float3 a, float3 b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
inline float3 cross(float3 a, float3 b) { return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x}; }
inline float3 normalize(float3 a) { return a*(1/std::sqrt(dot(a,a))); }
inline float3 max(float3 a, float3 b) { return {std::max(a.x,b.x),std::max(a.y,b.y),std::max(a.z,b.z)}; }
inline float max(float a, float b) { return std::max(a,b); }
inline float min(float a, float b) { return std::min(a,b); }
inline float saturate(float v) { return std::clamp(v,0.0f,1.0f); }
inline float lerp(float a, float b, float t) { return a+(b-a)*t; }
inline float3 lerp(float3 a, float3 b, float t) { return a+(b-a)*t; }
inline float smoothstep(float a, float b, float v) { float t=saturate((v-a)/(b-a)); return t*t*(3-2*t); }
inline float sign(float v) { return float((v>0)-(v<0)); }
using std::abs; using std::atan2; using std::cos; using std::exp; using std::exp2; using std::log; using std::log10; using std::log2;
using std::pow; using std::sin; using std::sqrt;
}
