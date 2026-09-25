#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "sdk/adapters/d3d12/d3d12_adapter.h"
#include <string>
#include <vector>
namespace ofps::core::gpu { struct Shaders { std::vector<char> vertex, pack, unpack, outline, warpPack, warpUnpack;
#define PW_TEMPORAL_PASS(name, member, reads, outputs, extent) std::vector<char> temporal##name;
#include "core/shaders/temporal_passes.def"
#undef PW_TEMPORAL_PASS
std::vector<char> temporalRefineModel, temporalFlowLumaModel;
bool loaded = false; bool tried = false; ofps::sdk::ShaderSet Set() const;
bool WarpLoaded() const { return !warpPack.empty() && !warpUnpack.empty(); }
bool TemporalLoaded() const {
#define PW_TEMPORAL_PASS(name, member, reads, outputs, extent) if (temporal##name.empty()) return false;
#include "core/shaders/temporal_passes.def"
#undef PW_TEMPORAL_PASS
return !temporalRefineModel.empty() && !temporalFlowLumaModel.empty(); } }; bool LoadShaders(const std::wstring &coreDirectory, Shaders &shaders); }
