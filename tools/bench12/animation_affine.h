#pragma once
#include "math.h"

struct Affine3x4 {
    float m[3][4]{};
    Affine3x4()=default;
    explicit Affine3x4(const Mat4 &matrix) {
        for(size_t r=0;r<3;++r) for(size_t c=0;c<4;++c) m[r][c]=matrix.m[r][c];
    }
    void Add(const Affine3x4 &matrix,float weight) {
        for(size_t r=0;r<3;++r) for(size_t c=0;c<4;++c) m[r][c]+=matrix.m[r][c]*weight;
    }
    Vec3 Direction(Vec3 p) const {
        return {m[0][0]*p.x+m[0][1]*p.y+m[0][2]*p.z,
                m[1][0]*p.x+m[1][1]*p.y+m[1][2]*p.z,
                m[2][0]*p.x+m[2][1]*p.y+m[2][2]*p.z};
    }
    Vec3 Point(Vec3 p) const { return Direction(p)+Vec3{m[0][3],m[1][3],m[2][3]}; }
};
