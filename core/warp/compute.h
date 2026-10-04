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

// Asks Unpack for detail transfer (colour filters 2/3): the frame the slot's last Pack read plus the
// model's edit. The colour and depth are the ones that Pack read, through its validated views, resting
// states and subresources (plane 0 of a planar depth-stencil), all remembered by the Pack.
// Lifetime: those are raw host resources, valid only during the evaluate that packed them. Unpack
// transfers only when the slot's Pack ran in the current evaluate (Ctx().evalCounter unchanged);
// otherwise it records the plain unpack and TransferApplied() is false.
struct TransferRequest {
    bool depthReversed = true; // near = 1 (reversed Z); standard Z is mirrored in the shader
};

// The colour-space rule of the transfer: the edit is computed and added in the numbers the shader
// reads, so the frame and the model's answer must be read through views of the same sRGB-ness. The
// core decides from it, before recording, whether the temporal base is replaced by the transfer.
bool TransferEligible(DXGI_FORMAT colorView, DXGI_FORMAT answerView);

// The parts of UnpackSupport that depend on the target alone, as ComputePath::Unpack fills them for a
// target `td` written through `targetView` into the path's `outputView`. The typed-store flags are the
// device's answers for those two views.
UnpackSupport TargetUnpackSupport(const D3D12_RESOURCE_DESC& td, DXGI_FORMAT targetView,
                                  DXGI_FORMAT outputView, std::uint32_t subresource,
                                  bool targetTypedStore, bool outputTypedStore);

// D3D12 adapter for the pure path policy. Source validation is repeated by Pack.
PackDecision ProbePack(ID3D12Device* device, RequestedPath request,
                       D3D12_COMMAND_LIST_TYPE listType, DXGI_FORMAT colorView,
                       bool privateCompute, bool shadersLoaded, std::string& reason);

class ComputePath final : public gpu::Disposable {
public:
    ~ComputePath() override;
    ComputePath(const ComputePath&) = delete;
    ComputePath& operator=(const ComputePath&) = delete;

    // `frameSlots` packed texture triplets, each with its native copy intermediate.
    static std::unique_ptr<ComputePath> Create(
        ID3D12Device* commandListDevice, const sdk::LayoutV2& layout,
        DXGI_FORMAT colorView, DXGI_FORMAT outputView,
        std::uint32_t frameSlots, std::string& reason);
    // Without `copyIntermediates` an Unpack whose target needs UnpackPath::CopyFromUav fails cleanly
    // (no GPU work recorded); the caller prepares them when such a target can occur (warp/texture_plan.h).
    static std::unique_ptr<ComputePath> Create(
        ID3D12Device* commandListDevice, const sdk::LayoutV2& layout,
        DXGI_FORMAT colorView, DXGI_FORMAT outputView,
        std::uint32_t frameSlots, bool copyIntermediates, std::string& reason);

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
    // transferRequest == nullptr is the plain unpack. A request transfers only for colour filters 2/3,
    // matching colour spaces, a Pack in the current evaluate, and a target and answer that are neither
    // the colour nor the depth the Pack read; otherwise the plain unpack runs.
    // TransferApplied() reports which one the last Unpack recorded.
    bool Unpack(ID3D12GraphicsCommandList* cmd, std::uint32_t frameSlot,
                ID3D12Resource* answer, DXGI_FORMAT answerView,
                const UnpackTarget& target, std::uint32_t outlines,
                float gain, float gamma, const OfpsFencePoint* privateUsePoint,
                const TransferRequest* transferRequest, std::string& reason);
    [[nodiscard]] bool TransferApplied() const noexcept;

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
