#pragma once

#if !defined(_WIN32)
#error The PeripheralWarp D3D11 adapter is available only on Windows.
#endif

#include "../common/adapter_types.h"
#include "peripheral_warp/input_v2.h"
#include "peripheral_warp/types.h"
#include "peripheral_warp/types_v2.h"

#include <d3d11.h>
#include <dxgiformat.h>
#include <memory>

namespace pw {

struct D3D11SourceViews {
    ID3D11ShaderResourceView *color;
    ID3D11ShaderResourceView *depth;
    ID3D11ShaderResourceView *motion;
    // For Pack, nullable when ConfigFlagInputConfidenceValid is clear; the shader
    // then emits neutral 1.0. For Unpack, provide it when restoring confidence.
    ID3D11ShaderResourceView *confidence;
};

struct D3D11TargetViews {
    ID3D11RenderTargetView *color;
    ID3D11RenderTargetView *depth;
    ID3D11RenderTargetView *motion;
    ID3D11RenderTargetView *confidence;
};

struct D3D11PackedViews {
    ID3D11ShaderResourceView *color;
    ID3D11ShaderResourceView *depth;
    ID3D11ShaderResourceView *motion;
    ID3D11ShaderResourceView *confidence;
};

class D3D11Adapter final {
public:
    D3D11Adapter();
    ~D3D11Adapter();
    D3D11Adapter(D3D11Adapter &&) noexcept;
    D3D11Adapter &operator=(D3D11Adapter &&) noexcept;
    D3D11Adapter(const D3D11Adapter &) = delete;
    D3D11Adapter &operator=(const D3D11Adapter &) = delete;

    [[nodiscard]] AdapterStatus Initialize(
        ID3D11Device *device,
        const LayoutV1 &layout,
        DXGI_FORMAT colorFormat,
        const ShaderSet &shaders);
    [[nodiscard]] AdapterStatus Initialize(
        ID3D11Device *device,
        const LayoutV2 &layout,
        DXGI_FORMAT colorFormat,
        const ShaderSet &shaders);
    void Shutdown() noexcept;

    // Executes one MRT pass. D3D11 state touched by the pass is restored before return.
    [[nodiscard]] AdapterStatus Pack(ID3D11DeviceContext *context, const D3D11SourceViews &sources) noexcept;
    [[nodiscard]] AdapterStatus PackV2(
        ID3D11DeviceContext *context,
        const D3D11SourceViews &sources,
        const InputDescriptionV2 &description) noexcept;

    // At least the color target must be non-null. Other outputs may be discarded.
    [[nodiscard]] AdapterStatus Unpack(
        ID3D11DeviceContext *context,
        const D3D11SourceViews &packedSources,
        const D3D11TargetViews &nativeTargets) noexcept;
    // Composites selected diagnostics into color inside the same Unpack draw.
    [[nodiscard]] AdapterStatus Unpack(
        ID3D11DeviceContext *context,
        const D3D11SourceViews &packedSources,
        const D3D11TargetViews &nativeTargets,
        DiagnosticOutlineFlags diagnosticOutlines) noexcept;
    [[nodiscard]] AdapterStatus DrawOutlines(
        ID3D11DeviceContext *context,
        ID3D11RenderTargetView *nativeColorTarget,
        DiagnosticOutlineFlags diagnosticOutlines) noexcept;
    // Legacy convenience wrapper for the cyan Center boundary.
    [[nodiscard]] AdapterStatus DrawCenterOutline(
        ID3D11DeviceContext *context,
        ID3D11RenderTargetView *nativeColorTarget) noexcept;

    [[nodiscard]] D3D11PackedViews PackedViews() const noexcept;
    [[nodiscard]] const LayoutV1 *Layout() const noexcept;
    [[nodiscard]] const LayoutV2 *LayoutV2Description() const noexcept;

    // Colour adjustment applied by Unpack: gain * pow(rgb, 1 / gamma); both 1.0 by default.
    // Takes effect from the next Unpack. Returns InvalidArgument for non-finite or non-positive
    // values.
    [[nodiscard]] AdapterStatus SetOutputColorAdjust(float gain, float gamma) noexcept;

private:
    [[nodiscard]] AdapterStatus InitializeInternal(
        ID3D11Device *device,
        const LayoutV2 &layout,
        const LayoutV1 *legacyLayout,
        DXGI_FORMAT colorFormat,
        const ShaderSet &shaders);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pw
