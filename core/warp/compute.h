#pragma once

#include "core/gpu/graveyard.h"
#include "core/warp/path_policy.h"
#include "optimizer_fps/input_v2.h"
#include "optimizer_fps/types_v2.h"
#include "sdk/adapters/d3d12/d3d12_adapter.h"

#include <d3d12.h>
#include <cstdint>
#include <memory>
#include <string>

namespace ofps::core::warp {

struct Packed {
    ID3D12Resource* color = nullptr;
    ID3D12Resource* depth = nullptr;
    ID3D12Resource* motion = nullptr;
};

struct UnpackTarget {
    ID3D12Resource* resource = nullptr;
    DXGI_FORMAT view = DXGI_FORMAT_UNKNOWN;
    D3D12_RESOURCE_STATES restState = D3D12_RESOURCE_STATE_COMMON;
    std::uint32_t subresource = 0;
    std::uint32_t x = 0, y = 0, width = 0, height = 0;
    bool temporalNeedsNativeRead = false;
};

// D3D12 adapter for the pure path policy. Source validation is repeated by Pack.
PackDecision ProbePack(ID3D12Device* device, RequestedPath request,
                       D3D12_COMMAND_LIST_TYPE listType, DXGI_FORMAT colorView,
                       bool privateCompute, bool shadersLoaded, std::string& reason);

class ComputePath final : public gpu::Disposable {
public:
    ~ComputePath() override;
    ComputePath(const ComputePath&) = delete;
    ComputePath& operator=(const ComputePath&) = delete;

    static std::unique_ptr<ComputePath> Create(
        ID3D12Device* commandListDevice, const sdk::LayoutV2& layout,
        DXGI_FORMAT colorView, DXGI_FORMAT outputView,
        std::uint32_t frameSlots, std::string& reason);

    bool Pack(ID3D12GraphicsCommandList* cmd, std::uint32_t frameSlot,
              const sdk::D3D12SourceResources& sources,
              const sdk::InputDescriptionV2& input,
              const D3D12_RESOURCE_STATES sourceRest[3],
              const UINT sourceSubresources[3],
              const OfpsFencePoint* privateUsePoint, Packed* out, std::string& reason);

    bool Unpack(ID3D12GraphicsCommandList* cmd, std::uint32_t frameSlot,
                ID3D12Resource* answer, DXGI_FORMAT answerView,
                const UnpackTarget& target, std::uint32_t outlines,
                float gain, float gamma, const OfpsFencePoint* privateUsePoint,
                std::string& reason);

    bool BaseUnpack(ID3D12GraphicsCommandList* cmd, std::uint32_t frameSlot,
                    const UnpackTarget& target, const OfpsFencePoint* privateUsePoint,
                    std::string& reason);

    [[nodiscard]] Packed PackedFor(std::uint32_t frameSlot) const noexcept;
    [[nodiscard]] D3D12_RESOURCE_STATES PackedRestState() const noexcept {
        return D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    }

private:
    ComputePath() = default;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ofps::core::warp
