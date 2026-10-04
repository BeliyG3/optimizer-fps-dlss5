// Windows and D3D11 headers for every pw_bench source, with the configuration they need first
// (lean windows.h, no min/max macros so std::min/std::max work).
#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;
