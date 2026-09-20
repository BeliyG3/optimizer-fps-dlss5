#include "image.h"
#include "device.h"
#include <wincodec.h>
#include <fstream>
#include <cmath>
#include <limits>

RgbaImage DecodeImage(const std::vector<uint8_t> &bytes)
{
    if(bytes.empty() || bytes.size()>MAXDWORD) throw std::runtime_error("Empty or oversized glTF image");
    ComPtr<IWICImagingFactory> wic; Check(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&wic)),"Create WIC factory");
    ComPtr<IWICStream> stream; ComPtr<IWICBitmapDecoder> decoder; ComPtr<IWICBitmapFrameDecode> frame; ComPtr<IWICFormatConverter> converter;
    Check(wic->CreateStream(&stream),"Create WIC stream");
    Check(stream->InitializeFromMemory(const_cast<BYTE *>(bytes.data()),DWORD(bytes.size())),"Load WIC stream");
    Check(wic->CreateDecoderFromStream(stream.Get(),nullptr,WICDecodeMetadataCacheOnDemand,&decoder),"Decode glTF image");
    Check(decoder->GetFrame(0,&frame),"Read WIC frame"); Check(wic->CreateFormatConverter(&converter),"Create WIC converter");
    Check(converter->Initialize(frame.Get(),GUID_WICPixelFormat32bppRGBA,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom),"Convert to RGBA8");
    RgbaImage image; Check(converter->GetSize(&image.width,&image.height),"Get image size");
    if(!image.width || !image.height || image.width>16384 || image.height>16384) throw std::runtime_error("Unsupported glTF image dimensions");
    image.pixels.resize(size_t(image.width)*image.height*4);
    Check(converter->CopyPixels(nullptr,image.width*4,UINT(image.pixels.size()),image.pixels.data()),"Copy WIC pixels"); return image;
}
HdrImage LoadHdr(const std::string &path)
{
    std::ifstream file(path,std::ios::binary|std::ios::ate);
    if(!file) throw std::runtime_error("Cannot open HDRI: "+path);
    auto size=file.tellg(); if(size<=0) throw std::runtime_error("Empty HDRI");
    std::vector<uint8_t> data(static_cast<size_t>(size)); file.seekg(0); file.read(reinterpret_cast<char *>(data.data()),size);
    if(!file) throw std::runtime_error("Cannot read HDRI");
    auto invalid=[]() { throw std::runtime_error("Invalid Radiance HDRI (requires -Y +X, flat RGBE or scanline RLE)"); };
    size_t p=0;
    auto line=[&]() { std::string s; while(p<data.size() && data[p]!='\n') { char c=char(data[p++]); if(c!='\r') s+=c; } if(p<data.size()) ++p; return s; };
    if(!line().starts_with("#?")) invalid();
    for(;;) { auto s=line(); if(s.empty()) break; if(p>=data.size()) invalid(); }
    int w=0,h=0; if(sscanf_s(line().c_str(),"-Y %d +X %d",&h,&w)!=2 || w<=0 || h<=0 || w>16384 || h>16384) invalid();
    HdrImage image; image.width=unsigned(w); image.height=unsigned(h); image.pixels.resize(size_t(w)*h*4);
    std::vector<uint8_t> scan(size_t(w)*4);
    for(int y=0;y<h;++y) {
        if(p+4>data.size()) invalid();
        const uint8_t *m=data.data()+p;
        if(m[0]==2 && m[1]==2 && ((int(m[2])<<8)|m[3])==w && w>=8 && w<0x8000) {
            p+=4;
            for(int c=0;c<4;++c) {
                int x=0;
                while(x<w) {
                    if(p>=data.size()) invalid(); int count=data[p++];
                    if(count>128) {
                        count-=128; if(p>=data.size() || x+count>w) invalid(); uint8_t v=data[p++];
                        while(count-->0) scan[size_t(c)*w+x++]=v;
                    } else {
                        if(!count || p+count>data.size() || x+count>w) invalid();
                        while(count-->0) scan[size_t(c)*w+x++]=data[p++];
                    }
                }
            }
        } else {
            if(p+size_t(w)*4>data.size()) invalid();
            for(int x=0;x<w;++x) for(int c=0;c<4;++c) scan[size_t(c)*w+x]=data[p+size_t(x)*4+c];
            p+=size_t(w)*4;
        }
        for(int x=0;x<w;++x) {
            int e=scan[size_t(w)*3+x]; float scale=e ? std::ldexp(1.0f/256,e-128) : 0;
            size_t dst=(size_t(y)*w+x)*4;
            for(int c=0;c<3;++c) image.pixels[dst+c]=(float(scan[size_t(c)*w+x])+0.5f)*scale;
            image.pixels[dst+3]=1;
        }
    }
    return image;
}
