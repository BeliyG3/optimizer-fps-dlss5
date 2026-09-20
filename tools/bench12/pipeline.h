#pragma once
#include "device.h"
std::vector<char> Shader(const char *name);
ComPtr<ID3D12RootSignature> Root(Device &device, const D3D12_ROOT_SIGNATURE_DESC &desc);
