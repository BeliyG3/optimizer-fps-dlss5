#pragma once

// Machine::RecordRaw's two ways of putting the host's colour into a texture (the spread schedule's raw
// snapshot, spread_passes.cpp): the reprojection pass with debugVis 3, which reads the colour through its
// view and stores it through the machine's typed view, and a plain copy of the colour's sub-rect. The
// copy is taken only where both leave the same bytes in the target, except the payload bits of a half-float
// NaN (a shader may change them; NaN stays NaN, and every reader tests isfinite); everything else keeps the pass.

#include <d3d12.h>

#include <cstdint>

namespace ofps::core::temporal {

// What RecordRaw wrote: the copy, the pass, or nothing (the descriptors had run out).
enum class RawRoute : std::uint8_t { Copy, Pass, None };

struct RawCopyCase {
    D3D12_RESOURCE_DESC color{};  // the host colour's resource
    D3D12_RESOURCE_DESC target{}; // the texture RecordRaw writes
    DXGI_FORMAT colorView = DXGI_FORMAT_UNKNOWN;   // the view the pass reads the colour with
    DXGI_FORMAT machineView = DXGI_FORMAT_UNKNOWN; // the view it stores with (the machine's interpolation target)
    UINT colorSubresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    std::uint32_t rectX = 0, rectY = 0, rectW = 0, rectH = 0; // the colour's sub-rect
    std::uint32_t nativeW = 0, nativeH = 0;                   // the machine's extent: what the pass writes
    float ratioScale = 0.0f; // > 0: the pass stores the colour log-encoded (PwTEncodeFrame), which no copy does
};

// True when a copy of the colour's sub-rect (subresource 0) leaves the bytes the pass would (NaN payloads aside).
bool RawCopyReproduces(const RawCopyCase &c);

} // namespace ofps::core::temporal
