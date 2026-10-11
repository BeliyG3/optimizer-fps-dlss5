#include "image_mips.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace {
float Linear(float v) { return v<=0.04045f ? v/12.92f : std::pow((v+0.055f)/1.055f,2.4f); }
// One level down: area box integration (the last row/column of odd-sized images included), summed per channel in
// source order. An 8-bit sRGB byte's linear value comes from a table of the same expression, bit for bit.
void Downsample(const ImageMip &previous, ImageMip &next, unsigned pixelBytes, bool srgb)
{
    static const auto table=[] { std::array<float,256> t{}; for(unsigned b=0;b<256;++b) t[b]=Linear(float(b)/255); return t; }();
    const unsigned width=previous.width, height=previous.height, w=next.width, h=next.height;
    for(unsigned y=0;y<h;++y) for(unsigned x=0;x<w;++x) {
        float sum[4]={}, weight=0;
        const float x0=float(x)*float(width)/float(w), x1=float(x+1)*float(width)/float(w);
        const float y0=float(y)*float(height)/float(h), y1=float(y+1)*float(height)/float(h);
        for(unsigned sy=unsigned(y0);sy<std::min(height,unsigned(std::ceil(y1)));++sy)
            for(unsigned sx=unsigned(x0);sx<std::min(width,unsigned(std::ceil(x1)));++sx) {
                const uint8_t *p=previous.bytes.data()+(size_t(sy)*width+sx)*pixelBytes;
                const float a=(std::min(x1,float(sx+1))-std::max(x0,float(sx)))*(std::min(y1,float(sy+1))-std::max(y0,float(sy)));
                for(unsigned c=0;c<4;++c) {
                    float v;
                    if(pixelBytes==16) { std::memcpy(&v,p+c*4,4); if(srgb && c<3) v=Linear(v); }
                    else v=srgb && c<3 ? table[p[c]] : float(p[c])/255;
                    sum[c]+=v*a;
                }
                weight+=a;
            }
        uint8_t *out=next.bytes.data()+(size_t(y)*w+x)*pixelBytes;
        for(unsigned c=0;c<4;++c) {
            float value=sum[c]/weight;
            if(srgb && c<3) value=value<=0.0031308f ? value*12.92f : 1.055f*std::pow(value,1/2.4f)-0.055f;
            if(pixelBytes==16) std::memcpy(out+c*4,&value,4);
            else out[c]=uint8_t(std::clamp(value*255+0.5f,0.0f,255.0f));
        }
    }
}
}

std::vector<ImageMip> BuildMips(unsigned width, unsigned height, unsigned pixelBytes, const void *data, bool srgb)
{
    if(!width || !height || !data || (pixelBytes!=4 && pixelBytes!=16)) throw std::runtime_error("Invalid mip input");
    std::vector<ImageMip> levels;
    ImageMip base{width,height,std::vector<uint8_t>(size_t(width)*height*pixelBytes)};
    std::memcpy(base.bytes.data(),data,base.bytes.size()); levels.push_back(std::move(base));
    while(width>1 || height>1) {
        unsigned w=std::max(1u,width/2), h=std::max(1u,height/2);
        ImageMip next{w,h,std::vector<uint8_t>(size_t(w)*h*pixelBytes)};
        Downsample(levels.back(),next,pixelBytes,srgb);
        levels.push_back(std::move(next)); width=w; height=h;
    }
    return levels;
}
