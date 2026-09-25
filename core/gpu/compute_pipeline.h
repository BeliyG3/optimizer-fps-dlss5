#pragma once

#include <d3d12.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace ofps::core::gpu {

// These builders accept the caller's layout. Temporal and warp have different root parameters.
bool CreateComputeRoot(ID3D12Device* device, const D3D12_ROOT_SIGNATURE_DESC& layout,
                       ID3D12RootSignature** out, std::string& reason);
bool CreateComputePass(ID3D12Device* device, ID3D12RootSignature* root,
                       const void* code, std::size_t bytes, const char* name,
                       ID3D12PipelineState** out, std::string& reason);

struct ComputePass {
    ID3D12PipelineState* pipeline = nullptr;
    const char* name = nullptr;
};

class ComputePipeline {
public:
    ComputePipeline() = default;
    ~ComputePipeline();
    ComputePipeline(const ComputePipeline&) = delete;
    ComputePipeline& operator=(const ComputePipeline&) = delete;

    bool Create(ID3D12Device* device, const void* packCode,
                std::size_t packBytes, const void* unpackCode,
                std::size_t unpackBytes);
    void Record(ID3D12GraphicsCommandList* cmd, ID3D12DescriptorHeap* descriptorHeap,
                D3D12_GPU_DESCRIPTOR_HANDLE table, const std::uint32_t outputRect[4],
                bool unpack, std::uint32_t width, std::uint32_t height) const;
    const std::string& Reason() const noexcept { return reason_; }
    bool Ready() const noexcept { return root_ && pack_.pipeline && unpack_.pipeline; }

private:
    void Reset() noexcept;
    ID3D12RootSignature* root_ = nullptr;
    ComputePass pack_, unpack_;
    std::string reason_;
};

} // namespace ofps::core::gpu
