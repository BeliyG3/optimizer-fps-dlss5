#pragma once

// Pack and the colour Unpack as compute dispatches, for hosts that evaluate the model on a compute
// command list (DLSS5-Reshade-AIO with asynchronous NGX compute). A compute list cannot record the
// SDK adapter's full-screen draws, so these record the same SDK texel functions
// (shaders/warp_pack_cs.hlsl, warp_unpack_cs.hlsl) through their own root signature and descriptor
// ring. The adapter still owns the packed textures and the direct-list path; nothing here runs on a
// direct list.
//
// Constants and descriptors are written into a ring far deeper than the host's queue, so a slot is
// never rewritten while the GPU may still read it. Everything is created through the command list's
// device and retired through the graveyard like the rest of the feature's GPU objects.

#include "ngx_common.h"

#include <array>
#include <cstdint>

namespace pwhook {

class WarpCompute final : public pwngx::Disposable {
public:
    ~WarpCompute() override;
    // false with a reason in `error` when the device refuses an object.
    bool Initialize(ID3D12Device *device, const pwngx::Shaders &shaders, char *error, std::size_t errorSize);

    // The host's native sources into the packed textures of `packed` (each in UNORDERED_ACCESS).
    // `packSlot` remembers the input constants for a later RecordUnpackPacked of the same slot.
    [[nodiscard]] pw::AdapterStatus RecordPack(ID3D12GraphicsCommandList *cmd, const pw::LayoutV2 &layout,
                                               const pw::D3D12SourceResources &sources,
                                               const pw::InputDescriptionV2 &input, const pw::D3D12PackedViews &packed,
                                               DXGI_FORMAT packedColorView, std::uint32_t packSlot);
    // Work-size sources (the model's output and the packed guides) into `target` (native, UNORDERED_ACCESS).
    [[nodiscard]] pw::AdapterStatus RecordUnpack(ID3D12GraphicsCommandList *cmd, const pw::LayoutV2 &layout,
                                                 const pw::D3D12SourceResources &workSources,
                                                 const pw::InputDescriptionV2 &input, ID3D12Resource *target,
                                                 DXGI_FORMAT targetView, std::uint32_t outlines, float gain, float gamma);
    // The packed textures of `packSlot` without the model (the temporal base), with the input
    // constants that packed the slot, as RecordUnpackOwnedColorFromSet does on a direct list.
    [[nodiscard]] pw::AdapterStatus RecordUnpackPacked(ID3D12GraphicsCommandList *cmd, const pw::LayoutV2 &layout,
                                                       const pw::D3D12PackedViews &packed, DXGI_FORMAT packedColorView,
                                                       std::uint32_t packSlot, ID3D12Resource *target, DXGI_FORMAT targetView,
                                                       float gain, float gamma);

private:
    static constexpr std::uint32_t kRing = 96;       // dispatches; three per evaluate at most
    static constexpr std::uint32_t kTableSize = 8;   // t0..t3, u0..u3
    static constexpr std::uint32_t kConstantSlot = 512; // warp constants at +0, input constants at +256
    static constexpr std::uint32_t kPackSlots = 8;

    struct Views {
        ID3D12Resource *resources[4];
        DXGI_FORMAT formats[4];
    };
    std::uint32_t NextSlot();
    void WriteConstants(std::uint32_t slot, const pw::LayoutV2 &layout, const pw::ShaderInputConstantsV2 &input);
    void WriteTable(std::uint32_t slot, const Views &srvs, const Views &uavs);
    void Dispatch(ID3D12GraphicsCommandList *cmd, ID3D12PipelineState *pso, std::uint32_t slot,
                  const std::uint32_t diagnostics[4], std::uint32_t width, std::uint32_t height);

    ID3D12Device *device_ = nullptr;
    ID3D12RootSignature *rootSignature_ = nullptr;
    ID3D12PipelineState *packPso_ = nullptr;
    ID3D12PipelineState *unpackPso_ = nullptr;
    ID3D12DescriptorHeap *heap_ = nullptr;
    ID3D12Resource *constants_ = nullptr;
    std::uint8_t *mapped_ = nullptr;
    UINT increment_ = 0;
    std::uint32_t next_ = 0;
    std::array<pw::ShaderInputConstantsV2, kPackSlots> packConstants_{};
    std::array<bool, kPackSlots> packConstantsValid_{};
};

} // namespace pwhook
