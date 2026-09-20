#include "image_mips.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

std::vector<ImageMip> BuildMips(unsigned width, unsigned height, unsigned pixelBytes, const void *data, bool srgb)
{
    if(!width || !height || !data || (pixelBytes!=4 && pixelBytes!=16)) throw std::runtime_error("Invalid mip input");
    std::vector<ImageMip> levels;
    ImageMip base{width,height,std::vector<uint8_t>(size_t(width)*height*pixelBytes)};
    std::memcpy(base.bytes.data(),data,base.bytes.size()); levels.push_back(std::move(base));
    while(width>1 || height>1) {
        unsigned w=std::max(1u,width/2), h=std::max(1u,height/2);
        ImageMip next{w,h,std::vector<uint8_t>(size_t(w)*h*pixelBytes)};
        const auto &previous=levels.back();
        // Area box integration includes the last row/column of odd-sized images.
        for(unsigned y=0;y<h;++y) for(unsigned x=0;x<w;++x) for(unsigned c=0;c<4;++c) {
            float sum=0, weight=0;
            float x0=float(x)*float(width)/float(w), x1=float(x+1)*float(width)/float(w);
            float y0=float(y)*float(height)/float(h), y1=float(y+1)*float(height)/float(h);
            for(unsigned sy=unsigned(y0);sy<std::min(height,unsigned(std::ceil(y1)));++sy)
                for(unsigned sx=unsigned(x0);sx<std::min(width,unsigned(std::ceil(x1)));++sx) {
                    size_t index=(size_t(sy)*width+sx)*pixelBytes+c*(pixelBytes/4);
                    float v=0;
                    if(pixelBytes==16) std::memcpy(&v,previous.bytes.data()+index,4);
                    else v=float(previous.bytes[index])/255;
                    if(srgb && c<3) v=v<=0.04045f ? v/12.92f : std::pow((v+0.055f)/1.055f,2.4f);
                    float a=(std::min(x1,float(sx+1))-std::max(x0,float(sx)))*(std::min(y1,float(sy+1))-std::max(y0,float(sy)));
                    sum+=v*a; weight+=a;
                }
            float value=sum/weight;
            if(srgb && c<3) value=value<=0.0031308f ? value*12.92f : 1.055f*std::pow(value,1/2.4f)-0.055f;
            size_t index=(size_t(y)*w+x)*pixelBytes+c*(pixelBytes/4);
            if(pixelBytes==16) std::memcpy(next.bytes.data()+index,&value,4);
            else next.bytes[index]=uint8_t(std::clamp(value*255+0.5f,0.0f,255.0f));
        }
        levels.push_back(std::move(next)); width=w; height=h;
    }
    return levels;
}
