// Minimal matrix math: row-major storage m[r][c], column-vector convention (clip = P * V * world),
// matching `mul(M, v)` in HLSL with row_major packing. pw_gltf.h builds on Vec3 / Mat4 from here.
#pragma once
#include <algorithm>
#include <cmath>

struct Vec3 { float x, y, z; };
inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 Cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline Vec3 Normalize(Vec3 a) { const float l = std::sqrt(Dot(a, a)); return l > 0 ? a * (1.0f / l) : a; }

struct Mat4 { float m[4][4]; };
inline Mat4 Identity() { Mat4 r{}; for (int i = 0; i < 4; ++i) r.m[i][i] = 1.0f; return r; }
inline Mat4 Mul(const Mat4 &a, const Mat4 &b)
{
    Mat4 r{};
    for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) for (int k = 0; k < 4; ++k) r.m[i][j] += a.m[i][k] * b.m[k][j];
    return r;
}
// Left-handed look-at (x right, y up, z into the screen).
inline Mat4 LookAt(Vec3 eye, Vec3 target)
{
    const Vec3 f = Normalize(target - eye);
    const Vec3 r = Normalize(Cross({0, 1, 0}, f));
    const Vec3 u = Cross(f, r);
    Mat4 v = Identity();
    v.m[0][0] = r.x; v.m[0][1] = r.y; v.m[0][2] = r.z; v.m[0][3] = -Dot(r, eye);
    v.m[1][0] = u.x; v.m[1][1] = u.y; v.m[1][2] = u.z; v.m[1][3] = -Dot(u, eye);
    v.m[2][0] = f.x; v.m[2][1] = f.y; v.m[2][2] = f.z; v.m[2][3] = -Dot(f, eye);
    return v;
}
// Reverse-Z perspective (depth 1 at the near plane, 0 at the far plane) with a sub-pixel jitter in
// pixels of the target (x right, y down), as games do for DLSS.
inline bool g_standardDepth = false; // --depth standard
inline Mat4 Perspective(float fovYDeg, float aspect, float n, float f, float jx, float jy, float w, float h)
{
    const float ys = 1.0f / std::tan(fovYDeg * 3.14159265f / 360.0f);
    const float xs = ys / aspect;
    Mat4 p{};
    p.m[0][0] = xs;
    p.m[1][1] = ys;
    p.m[0][2] = 2.0f * jx / w;
    p.m[1][2] = -2.0f * jy / h;
    p.m[2][2] = n / (n - f);
    p.m[2][3] = n * f / (f - n);
    p.m[3][2] = 1.0f;
    if (g_standardDepth) {
        // --depth standard: near = 0, far = 1. Everything past a few metres sits within a few percent of
        // 1, which is what broke a relative depth test in 007 First Light.
        p.m[2][2] = f / (f - n);
        p.m[2][3] = -n * f / (f - n);
    }
    return p;
}

// Orthographic projection with a normal 0..1 depth (the shadow pass, unlike the reverse-Z main pass).
inline Mat4 Ortho(float l, float r, float b, float t, float zn, float zf)
{
    Mat4 p{};
    p.m[0][0] = 2.0f / (r - l); p.m[0][3] = -(r + l) / (r - l);
    p.m[1][1] = 2.0f / (t - b); p.m[1][3] = -(t + b) / (t - b);
    p.m[2][2] = 1.0f / (zf - zn); p.m[2][3] = -zn / (zf - zn);
    p.m[3][3] = 1.0f;
    return p;
}

// General 4x4 inverse (Gauss-Jordan): the sky pass needs the inverse view-projection to turn a
// pixel back into a world-space view ray.
inline Mat4 Invert(const Mat4 &src)
{
    double a[4][8]{};
    for (int r = 0; r < 4; ++r) { for (int c = 0; c < 4; ++c) a[r][c] = src.m[r][c]; a[r][4 + r] = 1.0; }
    for (int c = 0; c < 4; ++c) {
        int piv = c;
        for (int r = c + 1; r < 4; ++r) if (std::fabs(a[r][c]) > std::fabs(a[piv][c])) piv = r;
        if (std::fabs(a[piv][c]) < 1e-20) return Identity();
        if (piv != c) for (int k = 0; k < 8; ++k) std::swap(a[c][k], a[piv][k]);
        const double inv = 1.0 / a[c][c];
        for (int k = 0; k < 8; ++k) a[c][k] *= inv;
        for (int r = 0; r < 4; ++r) if (r != c) { const double f = a[r][c]; if (f != 0.0) for (int k = 0; k < 8; ++k) a[r][k] -= f * a[c][k]; }
    }
    Mat4 out{};
    for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) out.m[r][c] = (float) a[r][4 + c];
    return out;
}
