#include "texture_cache.h"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>

namespace {
struct Header { char magic[4]; std::uint32_t version, srgb, levels; std::uint64_t key, encodedSize; };
std::filesystem::path FileOf(const std::filesystem::path &dir, std::uint64_t key, bool srgb)
{
    char name[40]; std::snprintf(name,sizeof(name),"%016llx_%c.mip",(unsigned long long)key,srgb ? 'c' : 'd');
    return dir/name;
}
}
std::uint64_t MipCacheKey(const std::vector<std::uint8_t> &encoded)
{
    std::uint64_t h=1469598103934665603ull;
    for(auto b:encoded) { h^=b; h*=1099511628211ull; }
    return h;
}
bool ReadMipCache(const std::filesystem::path &dir, std::uint64_t key, std::size_t encodedSize, bool srgb, std::vector<ImageMip> &levels)
{
    std::ifstream file(FileOf(dir,key,srgb),std::ios::binary);
    Header h{};
    if(!file || !file.read(reinterpret_cast<char *>(&h),sizeof(h)) || std::memcmp(h.magic,"OFMC",4) || h.version!=kMipCacheVersion ||
       h.key!=key || h.encodedSize!=encodedSize || h.srgb!=(srgb ? 1u : 0u) || !h.levels || h.levels>32) return false;
    std::vector<ImageMip> read(h.levels);
    for(auto &level:read) {
        std::uint32_t size[2]{};
        if(!file.read(reinterpret_cast<char *>(size),sizeof(size)) || !size[0] || !size[1] || size[0]>65536 || size[1]>65536) return false;
        level.width=size[0]; level.height=size[1]; level.bytes.resize(std::size_t(size[0])*size[1]*4);
        if(!file.read(reinterpret_cast<char *>(level.bytes.data()),std::streamsize(level.bytes.size()))) return false;
    }
    levels=std::move(read);
    return true;
}
void WriteMipCache(const std::filesystem::path &dir, std::uint64_t key, std::size_t encodedSize, bool srgb, const std::vector<ImageMip> &levels)
{
    std::error_code error; std::filesystem::create_directories(dir,error);
    const auto path=FileOf(dir,key,srgb);
    auto temporary=path; temporary+="."+std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id()))+".tmp";
    {
        std::ofstream file(temporary,std::ios::binary);
        Header h{{'O','F','M','C'},kMipCacheVersion,srgb ? 1u : 0u,std::uint32_t(levels.size()),key,encodedSize};
        file.write(reinterpret_cast<const char *>(&h),sizeof(h));
        for(const auto &level:levels) {
            const std::uint32_t size[2]{level.width,level.height};
            file.write(reinterpret_cast<const char *>(size),sizeof(size));
            file.write(reinterpret_cast<const char *>(level.bytes.data()),std::streamsize(level.bytes.size()));
        }
        if(!file) { file.close(); std::filesystem::remove(temporary,error); return; }
    }
    std::filesystem::rename(temporary,path,error);
    if(error) std::filesystem::remove(temporary,error);
}
