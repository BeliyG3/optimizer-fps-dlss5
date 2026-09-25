#include "core/gpu/compute_pipeline.h"

#include <d3dcompiler.h>
#include <wrl/client.h>

#include <cstring>
#include <string>

namespace ofps::core::gpu {
namespace {

std::string Failure(const char* action, HRESULT hr, ID3D12Device* device)
{
    std::string message = std::string(action) + " failed (HRESULT " + std::to_string(static_cast<unsigned long>(hr)) + ")";
    const HRESULT removed = device->GetDeviceRemovedReason();
    if (FAILED(removed))
        message += "; device removed (HRESULT " + std::to_string(static_cast<unsigned long>(removed)) + ")";
    return message;
}

bool CompleteDxbc(const void* code, std::size_t bytes)
{
    if (!code || bytes < 32 || std::memcmp(code, "DXBC", 4) != 0) return false;
    std::uint32_t declaredBytes = 0;
    std::memcpy(&declaredBytes, static_cast<const char*>(code) + 24, sizeof(declaredBytes));
    return declaredBytes >= 32 && declaredBytes <= bytes;
}

} // namespace

bool CreateComputeRoot(ID3D12Device* device, const D3D12_ROOT_SIGNATURE_DESC& layout,
                       ID3D12RootSignature** out, std::string& reason)
{
    if (!device || !out || *out) {
        reason = "root signature: invalid device or output";
        return false;
    }
    Microsoft::WRL::ComPtr<ID3DBlob> blob, errors;
    const HRESULT serialized = D3D12SerializeRootSignature(&layout, D3D_ROOT_SIGNATURE_VERSION_1,
                                                            &blob, &errors);
    if (FAILED(serialized) || !blob) {
        reason = Failure("root signature serialisation", serialized, device);
        if (errors && errors->GetBufferSize())
            reason += ": " + std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize());
        return false;
    }
    const HRESULT created = device->CreateRootSignature(0, blob->GetBufferPointer(),
                                                         blob->GetBufferSize(), IID_PPV_ARGS(out));
    if (FAILED(created)) {
        reason = Failure("root signature creation", created, device);
        return false;
    }
    return true;
}

bool CreateComputePass(ID3D12Device* device, ID3D12RootSignature* root,
                       const void* code, std::size_t bytes, const char* name,
                       ID3D12PipelineState** out, std::string& reason)
{
    const char* label = name ? name : "compute pass";
    if (!device || !root || !out || *out) {
        reason = std::string(label) + ": invalid device, root signature or output";
        return false;
    }
    if (!CompleteDxbc(code, bytes)) {
        reason = std::string(label) + ": missing or partial DXBC";
        return false;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = root;
    desc.CS = {code, bytes};
    const HRESULT created = device->CreateComputePipelineState(&desc, IID_PPV_ARGS(out));
    if (FAILED(created)) {
        reason = Failure(label, created, device);
        return false;
    }
    std::wstring wideName;
    for (const char* c = label; *c; ++c)
        wideName.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*c)));
    (*out)->SetName(wideName.c_str());
    return true;
}

ComputePipeline::~ComputePipeline() { Reset(); }

void ComputePipeline::Reset() noexcept
{
    if (unpack_.pipeline) unpack_.pipeline->Release();
    if (pack_.pipeline) pack_.pipeline->Release();
    if (root_) root_->Release();
    unpack_ = {};
    pack_ = {};
    root_ = nullptr;
}

bool ComputePipeline::Create(ID3D12Device* device, const void* packCode,
                             std::size_t packBytes, const void* unpackCode,
                             std::size_t unpackBytes)
{
    reason_.clear();
    if (root_) {
        reason_ = "compute pipeline already created";
        return false;
    }
    if (!device) {
        reason_ = "compute pipeline: missing device";
        return false;
    }
    // Validate both inputs before creating any D3D12 object.
    if (!CompleteDxbc(packCode, packBytes)) {
        reason_ = "CSPack: missing or partial DXBC";
        return false;
    }
    if (!CompleteDxbc(unpackCode, unpackBytes)) {
        reason_ = "CSUnpack: missing or partial DXBC";
        return false;
    }
    D3D12_DESCRIPTOR_RANGE ranges[3]{};
    ranges[0] = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 4, 0, 0, 0};
    ranges[1] = {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 3, 0, 0, 4};
    ranges[2] = {D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 3, 0, 0, 7};
    D3D12_ROOT_PARAMETER parameters[2]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[0].DescriptorTable = {3, ranges};
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[1].Constants = {3, 0, 4};
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_STATIC_SAMPLER_DESC samplers[2]{};
    for (unsigned i = 0; i < 2; ++i) {
        auto& sampler = samplers[i];
        sampler.Filter = i == 0 ? D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT
                                : D3D12_FILTER_MIN_MAG_MIP_POINT;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.MaxAnisotropy = 1;
        sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        sampler.ShaderRegister = i;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    D3D12_ROOT_SIGNATURE_DESC layout{};
    layout.NumParameters = 2;
    layout.pParameters = parameters;
    layout.NumStaticSamplers = 2;
    layout.pStaticSamplers = samplers;
    if (!CreateComputeRoot(device, layout, &root_, reason_) ||
        !CreateComputePass(device, root_, packCode, packBytes, "CSPack", &pack_.pipeline, reason_) ||
        !CreateComputePass(device, root_, unpackCode, unpackBytes, "CSUnpack", &unpack_.pipeline, reason_)) {
        Reset();
        return false;
    }
    pack_.name = "CSPack";
    unpack_.name = "CSUnpack";
    return true;
}

void ComputePipeline::Record(ID3D12GraphicsCommandList* cmd, ID3D12DescriptorHeap* descriptorHeap,
                             D3D12_GPU_DESCRIPTOR_HANDLE table, const std::uint32_t outputRect[4],
                             bool unpack, std::uint32_t width, std::uint32_t height) const
{
    ID3D12DescriptorHeap* heaps[] = {descriptorHeap};
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->SetComputeRootSignature(root_);
    cmd->SetPipelineState(unpack ? unpack_.pipeline : pack_.pipeline);
    cmd->SetComputeRootDescriptorTable(0, table);
    cmd->SetComputeRoot32BitConstants(1, 4, outputRect, 0);
    cmd->Dispatch((width + 15u) / 16u, (height + 7u) / 8u, 1);
}

} // namespace ofps::core::gpu
