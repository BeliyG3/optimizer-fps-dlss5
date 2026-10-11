#pragma once
#include "image.h"
#include "math.h"
#include <array>
#include <cmath>
#include <vector>

// --raster-sky-ambient: the HDRI sky as irradiance in nine spherical-harmonic coefficients (Ramamoorthi and Hanrahan,
// "An Efficient Representation for Irradiance Environment Maps"), so the raster shading gets an ambient term that
// depends on the surface normal: bright upward under an open sky, darker toward the ground.
// The coefficients are the sky's cosine-convolved projection: EvalSH(coefficients, n) is the irradiance
// integral(L(w) max(dot(n,w),0) dw) at a surface of normal n (pi * L for a constant sky L), times `scale`.
// shaders/raster_common.hlsli (PW_SKYAMB) evaluates the same nine terms; keep the basis and the order in step.

// The real SH basis in the paper's order: Y00, Y1-1 (y), Y10 (z), Y11 (x), Y2-2 (xy), Y2-1 (yz), Y20 (3zz-1), Y21 (xz), Y22 (xx-yy).
inline std::array<float,9> SkyShBasis(Vec3 n)
{
    return {0.282095f,0.488603f*n.y,0.488603f*n.z,0.488603f*n.x,1.092548f*n.x*n.y,1.092548f*n.y*n.z,
            0.315392f*(3*n.z*n.z-1),1.092548f*n.x*n.z,0.546274f*(n.x*n.x-n.y*n.y)};
}
// The projection of the whole sphere of the panorama, in the convention of Sky() in shaders/scene.hlsli: u = atan2(z,x)/2pi+0.5,
// v = acos(y)/pi, row 0 at the top (+y), texel centres, each texel weighted by its solid angle 2pi/w * pi/h * sin(theta).
// The lower hemisphere counts too, exactly as the path tracer's rays see it.
inline std::array<Vec3,9> SkyIrradianceSH(const HdrImage &image, float scale)
{
    constexpr double Pi=3.14159265358979323846;
    std::array<double,27> sum{};
    if(!image.width || !image.height || image.pixels.size()<size_t(image.width)*image.height*4) return {};
    std::vector<double> cosPhi(image.width), sinPhi(image.width);
    for(unsigned x=0;x<image.width;++x) {
        const double phi=((x+0.5)/image.width-0.5)*2*Pi; cosPhi[x]=std::cos(phi); sinPhi[x]=std::sin(phi);
    }
    for(unsigned y=0;y<image.height;++y) {
        const double theta=(y+0.5)/image.height*Pi, sinTheta=std::sin(theta), cosTheta=std::cos(theta);
        const double solidAngle=2*Pi/image.width*Pi/image.height*sinTheta;
        std::array<double,27> row{};
        for(unsigned x=0;x<image.width;++x) {
            const float *p=&image.pixels[(size_t(y)*image.width+x)*4];
            const Vec3 d{float(cosPhi[x]*sinTheta),float(cosTheta),float(sinPhi[x]*sinTheta)};
            const auto basis=SkyShBasis(d);
            for(int k=0;k<9;++k) for(int c=0;c<3;++c) row[k*3+c]+=double(p[c])*basis[k];
        }
        for(int i=0;i<27;++i) sum[i]+=row[i]*solidAngle;
    }
    // The cosine lobe's convolution: pi for the constant band, 2pi/3 for the linear one, pi/4 for the quadratic one.
    constexpr double band[9]={Pi,2*Pi/3,2*Pi/3,2*Pi/3,Pi/4,Pi/4,Pi/4,Pi/4,Pi/4};
    std::array<Vec3,9> out{};
    for(int k=0;k<9;++k) out[k]={float(sum[k*3]*band[k]*scale),float(sum[k*3+1]*band[k]*scale),float(sum[k*3+2]*band[k]*scale)};
    return out;
}
inline Vec3 EvalSH(const std::array<Vec3,9> &coefficients, Vec3 n)
{
    const auto basis=SkyShBasis(n); Vec3 sum{};
    for(int k=0;k<9;++k) sum=sum+coefficients[k]*basis[k];
    return sum;
}
