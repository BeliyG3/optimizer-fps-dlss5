#pragma once

// Row-vector 4x4 matrices in the D3DX conventions (left-handed), enough for a fixed-function scene.

#include <cmath>

namespace bench9 {

struct Mat {
    float m[16];
};

inline Mat Identity()
{
    Mat r{};
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
    return r;
}

inline Mat Mul(const Mat &a, const Mat &b)
{
    Mat r{};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            for (int k = 0; k < 4; ++k) r.m[i * 4 + j] += a.m[i * 4 + k] * b.m[k * 4 + j];
    return r;
}

inline Mat Translate(float x, float y, float z)
{
    Mat r = Identity();
    r.m[12] = x; r.m[13] = y; r.m[14] = z;
    return r;
}

inline Mat RotateY(float a)
{
    Mat r = Identity();
    r.m[0] = std::cos(a); r.m[2] = -std::sin(a); r.m[8] = std::sin(a); r.m[10] = std::cos(a);
    return r;
}

inline Mat RotateX(float a)
{
    Mat r = Identity();
    r.m[5] = std::cos(a); r.m[6] = std::sin(a); r.m[9] = -std::sin(a); r.m[10] = std::cos(a);
    return r;
}

// Left-handed look-at and perspective, the D3DX conventions (row vectors).
inline Mat LookAt(float ex, float ey, float ez, float tx, float ty, float tz)
{
    float zx = tx - ex, zy = ty - ey, zz = tz - ez;
    const float zl = std::sqrt(zx * zx + zy * zy + zz * zz);
    zx /= zl; zy /= zl; zz /= zl;
    float xx = zz, xy = 0.0f, xz = -zx; // up (0,1,0) x z
    const float xl = std::sqrt(xx * xx + xz * xz);
    xx /= xl; xz /= xl;
    const float yx = zy * xz - zz * xy, yy = zz * xx - zx * xz, yz = zx * xy - zy * xx;
    Mat r = Identity();
    r.m[0] = xx; r.m[1] = yx; r.m[2] = zx;
    r.m[4] = xy; r.m[5] = yy; r.m[6] = zy;
    r.m[8] = xz; r.m[9] = yz; r.m[10] = zz;
    r.m[12] = -(xx * ex + xy * ey + xz * ez);
    r.m[13] = -(yx * ex + yy * ey + yz * ez);
    r.m[14] = -(zx * ex + zy * ey + zz * ez);
    return r;
}

inline Mat Perspective(float fovY, float aspect, float zn, float zf)
{
    const float h = 1.0f / std::tan(fovY * 0.5f);
    Mat r{};
    r.m[0] = h / aspect;
    r.m[5] = h;
    r.m[10] = zf / (zf - zn);
    r.m[11] = 1.0f;
    r.m[14] = -zn * zf / (zf - zn);
    return r;
}

} // namespace bench9
