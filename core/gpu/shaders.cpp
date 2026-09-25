#include "core/gpu/shaders.h"
#include "core/log.h"
#include <fstream>
#include <iterator>

namespace ofps::core::gpu {
namespace {
bool ReadWholeFile(const std::wstring &path, std::vector<char> &out)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    out.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    return !out.empty();
}


} // namespace

ofps::sdk::ShaderSet Shaders::Set() const
{
    return ofps::sdk::ShaderSet{{vertex.data(), vertex.size()}, {pack.data(), pack.size()}, {unpack.data(), unpack.size()},
                         {outline.data(), outline.size()}};
}

bool LoadShaders(const std::wstring &coreDirectory, Shaders &s)
{
    if (s.tried) return s.loaded;
    s.tried = true;
    const std::wstring dir = coreDirectory + L"\\optimizer-fps-dlss5\\";
    Log(false, "Optimizer FPS core: shader directory: %ls", dir.c_str());
    s.loaded = ReadWholeFile(dir + L"fullscreen_vs.dxbc", s.vertex) && ReadWholeFile(dir + L"pack_ps.dxbc", s.pack) &&
               ReadWholeFile(dir + L"unpack_ps.dxbc", s.unpack);
    ReadWholeFile(dir + L"outline_ps.dxbc", s.outline);
    const bool packLoaded = ReadWholeFile(dir + L"warp_pack_cs.dxbc", s.warpPack);
    const bool unpackLoaded = ReadWholeFile(dir + L"warp_unpack_cs.dxbc", s.warpUnpack);
    if (!packLoaded) Log(true, "Optimizer FPS core: warp_pack_cs.dxbc was not found in optimizer-fps-dlss5\\; compute warp unavailable");
    if (!unpackLoaded) Log(true, "Optimizer FPS core: warp_unpack_cs.dxbc was not found in optimizer-fps-dlss5\\; compute warp unavailable");
#define PW_TEMPORAL_PASS(name, member, reads, outputs, extent) \
    ReadWholeFile(dir + L"temporal_" L## #name L"_cs.dxbc", s.temporal##name);
#include "core/shaders/temporal_passes.def"
#undef PW_TEMPORAL_PASS
    ReadWholeFile(dir + L"temporal_RefineModel_cs.dxbc", s.temporalRefineModel);
    ReadWholeFile(dir + L"temporal_FlowLumaModel_cs.dxbc", s.temporalFlowLumaModel);
    if (!s.loaded) Log(true, "Optimizer FPS NGX: shaders were not found in optimizer-fps-dlss5\\ beside the add-on");
    return s.loaded;
}

} // namespace ofps::core::gpu
