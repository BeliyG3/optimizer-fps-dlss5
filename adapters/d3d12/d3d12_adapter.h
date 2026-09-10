#pragma once

#if !defined(_WIN32)
#error The PeripheralWarp D3D12 adapter is available only on Windows.
#endif

#include "../common/adapter_types.h"
#include "peripheral_warp/input_v2.h"
#include "peripheral_warp/types.h"
#include "peripheral_warp/types_v2.h"

#include <d3d12.h>
#include <dxgiformat.h>
#include <memory>

namespace pw {

struct D3D12SourceResource {
    ID3D12Resource *resource;
    // Typed SRV format. When confidence is omitted, use UNKNOWN and a null resource.
    DXGI_FORMAT format;
};

struct D3D12SourceResources {
    D3D12SourceResource color;
    D3D12SourceResource depth;
    D3D12SourceResource motion;
    D3D12SourceResource confidence;
};

struct D3D12TargetFormats {
    DXGI_FORMAT color;
    DXGI_FORMAT depth;
    DXGI_FORMAT motion;
    DXGI_FORMAT confidence;
};

struct D3D12TargetHandles {
    D3D12_CPU_DESCRIPTOR_HANDLE color;
    D3D12_CPU_DESCRIPTOR_HANDLE depth;
    D3D12_CPU_DESCRIPTOR_HANDLE motion;
    D3D12_CPU_DESCRIPTOR_HANDLE confidence;
};

struct D3D12PackedViews {
    D3D12SourceResources resources;
    D3D12_CPU_DESCRIPTOR_HANDLE colorSrv;
    D3D12_CPU_DESCRIPTOR_HANDLE depthSrv;
    D3D12_CPU_DESCRIPTOR_HANDLE motionSrv;
    D3D12_CPU_DESCRIPTOR_HANDLE confidenceSrv;
    // Start of a contiguous four-SRV table in SourceDescriptorHeap().
    D3D12_GPU_DESCRIPTOR_HANDLE shaderResourceTable;
};

// The result of validating an ABI-v2 source set against a layout without touching any descriptor
// heap. An integrator that records its own fused Pack (compute, with its encode folded in) uses this
// to apply exactly the checks and constants the adapter would, so both paths reject the same inputs
// and warp identically.
struct D3D12SourceValidation {
    ShaderInputConstantsV2 constants;
    // Typed SRV formats to create for color, depth, motion and confidence, in that order. The
    // confidence entry is R16_FLOAT when no confidence resource was supplied.
    DXGI_FORMAT viewFormats[4];
    // The color rect matches the layout's native extent (Pack input) / work extent (Unpack input).
    bool nativeExtent;
    bool workExtent;
};

[[nodiscard]] AdapterStatus ValidateD3D12Sources(
    ID3D12Device *device,
    const LayoutV2 &layout,
    const D3D12SourceResources &sources,
    const InputDescriptionV2 &description,
    D3D12SourceValidation *validation) noexcept;

class D3D12Adapter final {
public:
    D3D12Adapter();
    ~D3D12Adapter();
    D3D12Adapter(D3D12Adapter &&) noexcept;
    D3D12Adapter &operator=(D3D12Adapter &&) noexcept;
    D3D12Adapter(const D3D12Adapter &) = delete;
    D3D12Adapter &operator=(const D3D12Adapter &) = delete;

    [[nodiscard]] AdapterStatus Initialize(
        ID3D12Device *device,
        const LayoutV1 &layout,
        const D3D12TargetFormats &packFormats,
        const D3D12TargetFormats &unpackFormats,
        const ShaderSet &shaders,
        std::uint32_t framesInFlight = 3,
        std::uint32_t sourceSets = 0); // see the LayoutV2 overload
    [[nodiscard]] AdapterStatus Initialize(
        ID3D12Device *device,
        const LayoutV2 &layout,
        const D3D12TargetFormats &packFormats,
        const D3D12TargetFormats &unpackFormats,
        const ShaderSet &shaders,
        std::uint32_t framesInFlight = 3,
        // Color-only NR consumers can preserve the four-descriptor table ABI while
        // discarding the pack shader's confidence output into null RTV/SRV views.
        bool allocateConfidence = true,
        // Number of external source sets (the four SRVs plus the input constants written by
        // WriteSourceDescriptors*). 0 = framesInFlight: one set per frame slot, addressed by the
        // frame slot as before. A larger ring lets a host that changes its input resources every
        // frame write a fresh set per frame without touching one the GPU may still be reading -
        // the texture slots are reused in queue order and need no such margin.
        std::uint32_t sourceSets = 0);
    void Shutdown() noexcept;
    [[nodiscard]] std::uint32_t SourceSetCount() const noexcept;

    // Colour adjustment applied by Unpack to the colour it writes: gain * pow(rgb, 1 / gamma) in
    // the target's own linear space. Both 1.0 by default; takes effect from the next Record*.
    [[nodiscard]] AdapterStatus SetOutputColorAdjust(float gain, float gamma) noexcept;

    // Call only when the selected frame slot is no longer in flight. The set must
    // consistently use either native extents (for RecordPack) or work extents (for
    // RecordUnpack). Confidence may be null only when ConfigFlagInputConfidenceValid
    // is clear in the initialized layout.
    [[nodiscard]] AdapterStatus WriteSourceDescriptors(
        std::uint32_t frameSlot,
        const D3D12SourceResources &sources) noexcept;
    // ABI-v2 path. Source resources may have independent extents and common
    // shader-readable typed formats. Pack normalizes guides while warping them.
    [[nodiscard]] AdapterStatus WriteSourceDescriptorsV2(
        std::uint32_t frameSlot,
        const D3D12SourceResources &sources,
        const InputDescriptionV2 &description) noexcept;
    // Source-set addressed variants (sourceSets > framesInFlight): the set index is independent
    // of the texture slot the draw packs into or unpacks from.
    [[nodiscard]] AdapterStatus WriteSourceSetV2(
        std::uint32_t sourceSet,
        const D3D12SourceResources &sources,
        const InputDescriptionV2 &description) noexcept;
    [[nodiscard]] AdapterStatus RecordPackFromSet(
        ID3D12GraphicsCommandList *commandList,
        std::uint32_t frameSlot,
        std::uint32_t sourceSet) noexcept;
    [[nodiscard]] AdapterStatus RecordPackFromSet(
        ID3D12GraphicsCommandList *commandList,
        std::uint32_t frameSlot,
        std::uint32_t sourceSet,
        const D3D12TargetHandles &workTargets) noexcept;
    [[nodiscard]] AdapterStatus RecordUnpackColorFromSet(
        ID3D12GraphicsCommandList *commandList,
        std::uint32_t frameSlot,
        std::uint32_t sourceSet,
        D3D12_CPU_DESCRIPTOR_HANDLE nativeColorTarget,
        DiagnosticOutlineFlags diagnosticOutlines) noexcept;
    // Owned packed textures of `frameSlot` with the input constants of `sourceSet` (the set that
    // packed the slot).
    [[nodiscard]] AdapterStatus RecordUnpackOwnedColorFromSet(
        ID3D12GraphicsCommandList *commandList,
        std::uint32_t frameSlot,
        std::uint32_t sourceSet,
        D3D12_CPU_DESCRIPTOR_HANDLE nativeColorTarget,
        DiagnosticOutlineFlags diagnosticOutlines) noexcept;

    // The caller owns resource barriers and fence/timeline synchronization. These methods only
    // record a full-screen draw into an already open direct command list. They replace the
    // command list's descriptor heap, root signature, pipeline state, root arguments, viewport,
    // scissor, primitive topology, and render targets. D3D12 provides no getters for restoring
    // that state, so the caller must explicitly rebind every graphics state it needs afterwards.
    [[nodiscard]] AdapterStatus RecordPack(
        ID3D12GraphicsCommandList *commandList,
        std::uint32_t frameSlot,
        const D3D12TargetHandles &workTargets) noexcept;
    // Records into the adapter-owned work-size textures for this frame slot.
    [[nodiscard]] AdapterStatus RecordPack(
        ID3D12GraphicsCommandList *commandList,
        std::uint32_t frameSlot) noexcept;
    [[nodiscard]] AdapterStatus RecordUnpack(
        ID3D12GraphicsCommandList *commandList,
        std::uint32_t frameSlot,
        const D3D12TargetHandles &nativeTargets) noexcept;
    // Same pass as RecordUnpack, with transient diagnostic outlines composited
    // into color after reconstruction. No extra full-screen draw is recorded.
    [[nodiscard]] AdapterStatus RecordUnpack(
        ID3D12GraphicsCommandList *commandList,
        std::uint32_t frameSlot,
        const D3D12TargetHandles &nativeTargets,
        DiagnosticOutlineFlags diagnosticOutlines) noexcept;
    [[nodiscard]] AdapterStatus RecordUnpackColor(
        ID3D12GraphicsCommandList *commandList,
        std::uint32_t frameSlot,
        D3D12_CPU_DESCRIPTOR_HANDLE nativeColorTarget) noexcept;
    [[nodiscard]] AdapterStatus RecordUnpackColor(
        ID3D12GraphicsCommandList *commandList,
        std::uint32_t frameSlot,
        D3D12_CPU_DESCRIPTOR_HANDLE nativeColorTarget,
        DiagnosticOutlineFlags diagnosticOutlines) noexcept;
    // Uses the adapter-owned packed textures as inputs without rewriting the
    // selected frame slot's external source descriptors.
    [[nodiscard]] AdapterStatus RecordUnpackOwned(
        ID3D12GraphicsCommandList *commandList,
        std::uint32_t frameSlot,
        const D3D12TargetHandles &nativeTargets) noexcept;
    [[nodiscard]] AdapterStatus RecordUnpackOwned(
        ID3D12GraphicsCommandList *commandList,
        std::uint32_t frameSlot,
        const D3D12TargetHandles &nativeTargets,
        DiagnosticOutlineFlags diagnosticOutlines) noexcept;
    [[nodiscard]] AdapterStatus RecordUnpackOwnedColor(
        ID3D12GraphicsCommandList *commandList,
        std::uint32_t frameSlot,
        D3D12_CPU_DESCRIPTOR_HANDLE nativeColorTarget) noexcept;
    [[nodiscard]] AdapterStatus RecordUnpackOwnedColor(
        ID3D12GraphicsCommandList *commandList,
        std::uint32_t frameSlot,
        D3D12_CPU_DESCRIPTOR_HANDLE nativeColorTarget,
        DiagnosticOutlineFlags diagnosticOutlines) noexcept;
    // Compatibility draw for hosts that cannot select the integrated Unpack
    // overload. Center and raw Work are still emitted together in one draw.
    [[nodiscard]] AdapterStatus RecordOutlines(
        ID3D12GraphicsCommandList *commandList,
        std::uint32_t frameSlot,
        D3D12_CPU_DESCRIPTOR_HANDLE nativeColorTarget,
        DiagnosticOutlineFlags diagnosticOutlines) noexcept;
    // Legacy convenience wrapper for the cyan Center boundary.
    [[nodiscard]] AdapterStatus RecordCenterOutline(
        ID3D12GraphicsCommandList *commandList,
        std::uint32_t frameSlot,
        D3D12_CPU_DESCRIPTOR_HANDLE nativeColorTarget) noexcept;

    [[nodiscard]] ID3D12DescriptorHeap *SourceDescriptorHeap() const noexcept;
    [[nodiscard]] D3D12PackedViews PackedViews(std::uint32_t frameSlot) const noexcept;
    [[nodiscard]] const LayoutV1 *Layout() const noexcept;
    [[nodiscard]] const LayoutV2 *LayoutV2Description() const noexcept;

private:
    [[nodiscard]] AdapterStatus InitializeInternal(
        ID3D12Device *device,
        const LayoutV2 &layout,
        const LayoutV1 *legacyLayout,
        const D3D12TargetFormats &packFormats,
        const D3D12TargetFormats &unpackFormats,
        const ShaderSet &shaders,
        std::uint32_t framesInFlight,
        bool allocateConfidence,
        std::uint32_t sourceSets);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pw
