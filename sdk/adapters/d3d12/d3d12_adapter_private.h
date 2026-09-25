#pragma once
#include "d3d12_adapter.h"
#include <array>
#include <vector>
#include <wrl/client.h>

namespace ofps::sdk {
using Microsoft::WRL::ComPtr;

struct D3D12Adapter::Impl {
    std::uint32_t outputGainBits = 0;     // float bits of the Unpack colour gain (0 = unset)
    std::uint32_t outputInvGammaBits = 0; // float bits of 1 / gamma (0 = unset)
    enum SourceExtent : std::uint8_t {
        SourceExtentNone = 0,
        SourceExtentNative = 1u << 0,
        SourceExtentWork = 1u << 1,
    };

    struct PackedFrame {
        std::array<ComPtr<ID3D12Resource>, 4> resources;
        D3D12TargetHandles rtvs{};
        std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 4> srvs{};
        D3D12_GPU_DESCRIPTOR_HANDLE shaderResourceTable{};
    };

    LayoutV2 layout{};
    LayoutV1 legacyLayout{};
    bool hasLegacyLayout = false;
    bool packedConfidenceAllocated = true;
    std::uint32_t framesInFlight = 0;
    std::uint32_t sourceSets = 0; // external source sets (>= framesInFlight); the owned SRV tables follow them in the heap
    UINT descriptorIncrement = 0;
    UINT rtvDescriptorIncrement = 0;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12RootSignature> rootSignature;
    ComPtr<ID3D12PipelineState> packPipeline;
    ComPtr<ID3D12PipelineState> unpackPipeline;
    ComPtr<ID3D12PipelineState> unpackColorPipeline;
    ComPtr<ID3D12PipelineState> outlinePipeline;
    ComPtr<ID3D12DescriptorHeap> sourceHeap;
    ComPtr<ID3D12DescriptorHeap> packedRtvHeap;
    ComPtr<ID3D12Resource> constants;
    ComPtr<ID3D12Resource> inputConstants;
    std::vector<std::uint8_t> sourceExtents;
    std::vector<PackedFrame> packedFrames;

    [[nodiscard]] D3D12_GPU_DESCRIPTOR_HANDLE ExternalSourceTable(std::uint32_t sourceSet) const noexcept
    {
        D3D12_GPU_DESCRIPTOR_HANDLE table = sourceHeap->GetGPUDescriptorHandleForHeapStart();
        table.ptr += static_cast<UINT64>(sourceSet) * 4u * descriptorIncrement;
        return table;
    }

    AdapterStatus CreatePipeline(const ShaderSet &shaders, ShaderBytecode pixel,
                                 const D3D12TargetFormats &formats, UINT targetCount,
                                 ID3D12PipelineState **output, bool confidenceTarget = true) noexcept;
    AdapterStatus CreateResources(const D3D12TargetFormats &packFormats,
                                  const LayoutV2 &layout, std::uint32_t framesInFlight,
                                  std::uint32_t sourceSets, bool allocateConfidence) noexcept;
    AdapterStatus Record(ID3D12GraphicsCommandList *list, std::uint32_t constantSet,
                         ID3D12PipelineState *pipeline, const D3D12_CPU_DESCRIPTOR_HANDLE *targets,
                         UINT targetCount, std::uint32_t width, std::uint32_t height,
                         D3D12_GPU_DESCRIPTOR_HANDLE sourceTable,
                         DiagnosticOutlineFlags diagnosticOutlines = DiagnosticOutlineNone) noexcept;
};

} // namespace ofps::sdk
