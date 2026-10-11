#include "tests_raster_support.h"
#include "image.h"
#include "device.h"
#include <fstream>
using namespace raster_tests;
namespace {
std::vector<uint8_t> ReadFile(const std::filesystem::path &path)
{
    std::ifstream file(path,std::ios::binary|std::ios::ate); Require(bool(file),"dump test: cannot open file");
    std::vector<uint8_t> bytes(size_t(file.tellg())); file.seekg(0); file.read(reinterpret_cast<char *>(bytes.data()),std::streamsize(bytes.size()));
    return bytes;
}
}
// --dump-format: the option, and that a PNG dump decodes to the BMP dump's pixels.
void DumpChecks(const std::filesystem::path &scratch)
{
    Require(Parse({"bench"}).dumpFormat=="bmp","--dump-format defaults to bmp");
    Require(Parse({"bench","10","--dump","1","--dump-format","png"}).dumpFormat=="png","--dump-format png");
    Require(Parse({"bench","--dump-format","bmp"}).dumpFormat=="bmp","--dump-format bmp");
    Require(Rejected({"bench","--dump-format","jpg"}) && Rejected({"bench","--dump-format","PNG"}) && Rejected({"bench","--dump-format"}),"--dump-format rejects other values");
    // A synthetic frame (not a multiple of any block size) with a smooth ramp, noise and hard edges; alpha is 255 as in WriteDump.
    constexpr unsigned w=157, h=91;
    std::vector<uint8_t> bgra(size_t(w)*h*4); uint32_t seed=12345;
    for(unsigned y=0;y<h;++y) for(unsigned x=0;x<w;++x) {
        seed=seed*1664525u+1013904223u; uint8_t *p=&bgra[(size_t(y)*w+x)*4];
        p[0]=uint8_t(x*255/(w-1)); p[1]=uint8_t(y*255/(h-1)); p[2]=uint8_t(seed>>24); p[3]=255;
        if(x==0 || y==0) { p[0]=255; p[1]=0; p[2]=7; }
    }
    const auto bmp=scratch/"dump_format_test.bmp", png=scratch/"dump_format_test.png";
    Check(CoInitializeEx(nullptr,COINIT_MULTITHREADED),"Initialize WIC dump test");
    WriteBmp(bmp,w,h,bgra.data()); WritePng(png,w,h,bgra.data());
    const auto bmpBytes=ReadFile(bmp);
    Require(bmpBytes.size()==54+bgra.size() && std::equal(bgra.begin(),bgra.end(),bmpBytes.begin()+54),"BMP dump holds the BGRA pixels");
    const auto decoded=DecodeImage(ReadFile(png)); // RGBA
    CoUninitialize();
    Require(decoded.width==w && decoded.height==h,"PNG dump size");
    bool equal=true;
    for(size_t i=0;i<size_t(w)*h && equal;++i) {
        const uint8_t *s=&bgra[i*4], *d=&decoded.pixels[i*4];
        equal=d[0]==s[2] && d[1]==s[1] && d[2]==s[0] && d[3]==255; // the same pixel as the BMP's, RGB order, opaque
    }
    Require(equal,"PNG dump decodes to the BMP dump's pixels");
    std::error_code ignored; std::filesystem::remove(bmp,ignored); std::filesystem::remove(png,ignored);
    std::printf("[test] dump format: PNG pixels equal BMP pixels (%ux%u), option parsed\n",w,h);
}
