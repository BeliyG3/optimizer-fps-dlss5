#pragma once
#include <algorithm>
#include <cmath>

struct Vec3 { float x = 0, y = 0, z = 0; };
inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x+b.x, a.y+b.y, a.z+b.z}; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x-b.x, a.y-b.y, a.z-b.z}; }
inline Vec3 operator*(Vec3 a, float s) { return {a.x*s, a.y*s, a.z*s}; }
inline float Dot(Vec3 a, Vec3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
inline Vec3 Cross(Vec3 a, Vec3 b) { return {a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x}; }
inline Vec3 Normalize(Vec3 v) { float l = std::sqrt(Dot(v,v)); return l > 0 ? v*(1/l) : Vec3{}; }
struct Mat4 { float m[4][4]{}; };
inline Mat4 Identity() { Mat4 r; for (int i=0; i<4; ++i) r.m[i][i]=1; return r; }
inline Mat4 Mul(const Mat4 &a, const Mat4 &b)
{
    Mat4 r;
    for (int i=0; i<4; ++i) for (int j=0; j<4; ++j) for (int k=0; k<4; ++k) r.m[i][j]+=a.m[i][k]*b.m[k][j];
    return r;
}
inline Mat4 LookAt(Vec3 eye, Vec3 target)
{
    Vec3 f=Normalize(target-eye), r=Normalize(Cross({0,1,0},f)), u=Cross(f,r);
    Mat4 v=Identity(); Vec3 axes[]={r,u,f};
    for (int i=0; i<3; ++i) { v.m[i][0]=axes[i].x; v.m[i][1]=axes[i].y; v.m[i][2]=axes[i].z; v.m[i][3]=-Dot(axes[i],eye); }
    return v;
}
inline Mat4 Perspective(float fov, float aspect, bool reverse)
{
    constexpr float n=0.1f, f=300.0f;
    Mat4 p; p.m[1][1]=1/std::tan(fov*3.14159265f/360); p.m[0][0]=p.m[1][1]/aspect;
    p.m[2][2]=reverse ? n/(n-f) : f/(f-n); p.m[2][3]=reverse ? n*f/(f-n) : -n*f/(f-n); p.m[3][2]=1;
    return p;
}
inline float Halton(unsigned n, unsigned base)
{
    float value=0, fraction=1;
    while (n) { fraction/=float(base); value+=fraction*float(n%base); n/=base; }
    return value;
}
