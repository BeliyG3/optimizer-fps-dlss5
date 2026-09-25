#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
#include <cstdint>
#include <string>
namespace ofps::reshade { using PFN_Create = int(__cdecl *)(ID3D12GraphicsCommandList *, int, void *, void **); using PFN_Evaluate = int(__cdecl *)(ID3D12GraphicsCommandList *, void *, void *, void *); using PFN_Release = int(__cdecl *)(void *); struct HookShim { PFN_Create realCreate = nullptr; PFN_Evaluate realEvaluate = nullptr; PFN_Release realRelease = nullptr; std::uint32_t hookAttempts = 0; ULONGLONG hookFirstPoll = 0; bool hookDelayLogged = false;
}; HookShim &Shim(); bool LoadForwarder(const std::wstring &directory, std::string *error); bool ForwarderLoaded(); bool DescribeExportPrologue(const char *hookName, const char *exportName, void *fn); int CallCreate(ID3D12GraphicsCommandList *cmd, int featureId, void *params, void **outHandle); int CallEvaluate(ID3D12GraphicsCommandList *cmd, void *handle, void *params, void *callback); int CallRelease(void *handle); }
