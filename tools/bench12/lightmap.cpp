#include "lightmap.h"
#include "image.h"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace {
std::runtime_error Failure(const std::string &path, const std::string &reason)
{
    return std::runtime_error("Cannot open lightmap "+path+": "+reason);
}
// One map of an atlas as floats, checked against the JSON's size.
HdrImage LoadAtlas(const std::filesystem::path &path, unsigned size)
{
    const std::string name=path.string();
    // Checked here so LoadHdr's own "Cannot open HDRI: <path>" never repeats the path.
    if(!std::ifstream(path,std::ios::binary)) throw Failure(name,"the file is missing or unreadable");
    HdrImage image;
    try { image=LoadHdr(name); } catch(const std::exception &error) { throw Failure(name,error.what()); }
    if(image.width!=size || image.height!=size)
        throw Failure(name,"it is "+std::to_string(image.width)+"x"+std::to_string(image.height)+", the JSON says "+std::to_string(size));
    return image;
}
// One map of an atlas: the Radiance file as R9G9B9E5 texels (4 bytes, a quarter of RGBA32F), one mip. `added`: a second
// map summed in floats before the packing (a "lamp" map into rest without --lamp-shadows).
ComPtr<ID3D12Resource> UploadAtlas(Device &d, const std::filesystem::path &path, unsigned size, const std::filesystem::path &added={})
{
    HdrImage image=LoadAtlas(path,size);
    if(!added.empty()) {
        const HdrImage more=LoadAtlas(added,size);
        for(size_t i=0;i<image.pixels.size();++i) image.pixels[i]+=more.pixels[i];
    }
    std::vector<ImageMip> level(1);
    level[0].width=image.width; level[0].height=image.height; level[0].bytes.resize(size_t(image.width)*image.height*4);
    uint8_t *out=level[0].bytes.data();
    for(size_t i=0,count=size_t(image.width)*image.height;i<count;++i) {
        const uint32_t texel=PackR9G9B9E5(image.pixels[i*4],image.pixels[i*4+1],image.pixels[i*4+2]);
        std::memcpy(out+i*4,&texel,4);
    }
    image.pixels=std::vector<float>(); // 1 GB at 8192^2: released before the upload buffer is created
    return d.UploadMips(DXGI_FORMAT_R9G9B9E5_SHAREDEXP,4,level);
}
}

LightmapSet LoadLightmap(Device &d, const std::string &jsonPath, unsigned sceneAtlases, bool lampMaps)
{
    const auto start=std::chrono::steady_clock::now();
    std::ifstream file(jsonPath,std::ios::binary);
    if(!file) throw Failure(jsonPath,"the file is missing or unreadable");
    std::ostringstream text; text<<file.rdbuf();
    LightmapSet lightmap;
    try { lightmap.bake=ParseLightmapJson(text.str()); } catch(const std::exception &error) { throw Failure(jsonPath,error.what()); }
    const auto &b=lightmap.bake;
    if(sceneAtlases>b.atlases.size())
        throw Failure(jsonPath,"the scene's lightmap UVs address "+std::to_string(sceneAtlases)+" atlases, the JSON lists "+std::to_string(b.atlases.size()));
    lightmap.rest=!b.atlases.front().rest.empty(); // all atlases or none (ParseLightmapJson), as "lamp"
    const bool lamp=!b.atlases.front().lamp.empty();
    if(lampMaps && !lamp)
        throw Failure(jsonPath,"--lamp-shadows needs a lightmap with \"lamp\" maps (bake_lab_lightmaps.py from 2026-10-07 on)");
    const auto folder=std::filesystem::path(jsonPath).parent_path();
    std::string sizes;
    for(const auto &atlas:b.atlases) {
        const std::filesystem::path folded=lamp && !lampMaps ? folder/atlas.lamp : std::filesystem::path();
        lightmap.full.push_back(UploadAtlas(d,folder/(lightmap.rest ? atlas.rest : atlas.full),atlas.size,folded));
        lightmap.sun.push_back(UploadAtlas(d,folder/atlas.sun,atlas.size));
        if(lampMaps) lightmap.lamp.push_back(UploadAtlas(d,folder/atlas.lamp,atlas.size));
        sizes+=(sizes.empty() ? "" : ", ")+std::to_string(atlas.size)+"^2";
    }
    // UploadMips leaves textures for non-pixel stages; the raster scene pass samples these in its pixel shader.
    d.Begin();
    std::vector<ID3D12Resource *> maps;
    for(size_t i=0;i<lightmap.full.size();++i) { maps.push_back(lightmap.full[i].Get()); maps.push_back(lightmap.sun[i].Get()); }
    for(const auto &map:lightmap.lamp) maps.push_back(map.Get());
    for(auto *map:maps) Transition(d.list.Get(),map,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE|D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    d.Submit();
    // full and sun of atlas i at 2i and 2i+1, then the lamp maps (--lamp-shadows) at 2n+i: the order of `maps`.
    lightmap.srvBase=d.Allocate(unsigned(maps.size()));
    for(size_t i=0;i<maps.size();++i) d.TextureSrv(maps[i],lightmap.srvBase+unsigned(i));
    std::printf("[lightmap] %zu atlas(es) (%s, %s%s and sun) from %s, scale full %.4g sun %.4g, baked sun %.3g,%.3g,%.3g x %.4g: %.1f s\n",
        b.atlases.size(),sizes.c_str(),lightmap.rest ? "rest" : "full",lamp ? (lampMaps ? ", lamp" : " + lamp") : "",jsonPath.c_str(),b.scaleFull,b.scaleSun,b.sunDir.x,b.sunDir.y,b.sunDir.z,
        b.sunStrength,std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count());
    return lightmap;
}
