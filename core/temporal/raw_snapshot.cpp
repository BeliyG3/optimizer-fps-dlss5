#include "core/temporal/raw_snapshot.h"

#include "core/temporal/machine.h"
#include "core/temporal/resources.h"

namespace ofps::core::temporal {
namespace {

using ofps::core::gpu::BarrierExternal;

// Views whose load into a shader and store back return every stored bit pattern unchanged, except the payload of
// a half-float NaN, which a shader may change (it stays a NaN): UNORM converts exactly both ways, and half floats
// widen to float32 and narrow back exactly, denormals included (a half denormal is a normal float32). Left out: sRGB (a curve each way), SNORM (two
// codes for -1), X channels (the store drops them), float32 (a shader may flush its denormals) and the
// 11/10-bit floats (their denormals on a store are not pinned down for every GPU).
bool RoundTripsExactly(DXGI_FORMAT view)
{
    switch (view) {
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_UNORM:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
        return true;
    default:
        return false;
    }
}

} // namespace

bool RawCopyReproduces(const RawCopyCase &c)
{
    // The pass reads the colour's first subresource (a TEXTURE2D view, mip 0) at native pixel p from
    // rect.xy + p when the sub-rect is the native size, so the copy's box is that sub-rect.
    const bool firstSubresource = c.colorSubresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES || c.colorSubresource == 0;
    const bool plainTexture = c.color.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && c.color.SampleDesc.Count == 1 &&
                              c.color.DepthOrArraySize == 1;
    const bool oneToOne = c.nativeW != 0 && c.nativeH != 0 && c.rectW == c.nativeW && c.rectH == c.nativeH &&
                          c.rectX <= c.color.Width && c.rectW <= c.color.Width - c.rectX &&
                          c.rectY <= c.color.Height && c.rectH <= c.color.Height - c.rectY;
    // ratioScale <= 0: PwTEncodeFrame passes the colour through (a NaN scale is refused as well).
    return c.ratioScale <= 0.0f && c.colorView == c.machineView && RoundTripsExactly(c.colorView) &&
           c.color.Format == c.target.Format && firstSubresource && plainTexture && oneToOne;
}

RawRoute Machine::RecordRaw(ID3D12GraphicsCommandList *cmd, const FrameInputs &in, ID3D12Resource *target,
                        D3D12_RESOURCE_STATES state, std::uint32_t dstX, std::uint32_t dstY, UINT outputSubresource)
{
    Resources &m = *res_;
    RawCopyCase c;
    c.color = in.color->GetDesc();
    c.target = target->GetDesc();
    c.colorView = in.colorView;
    c.machineView = m.outputView;
    c.colorSubresource = in.colorSubresource;
    c.rectX = in.colorRect.x; c.rectY = in.colorRect.y; c.rectW = in.colorRect.w; c.rectH = in.colorRect.h;
    c.nativeW = m.nativeW; c.nativeH = m.nativeH;
    c.ratioScale = in.ratioScale;
    if (in.color != target && RawCopyReproduces(c)) {
        // With its descriptors gone the pass writes nothing (RecordReproject); neither does the copy.
        if (m.exhausted) return RawRoute::None;
        CopyRaw(cmd, in, target, state, dstX, dstY, outputSubresource);
        return RawRoute::Copy;
    }
    const auto phase = m.phase;
    FrameInputs raw = in;
    raw.base = nullptr;
    raw.debugVis = 3;
    raw.phaseInFrames = 0;
    Reproject(cmd, raw, target, state, dstX, dstY, outputSubresource, false);
    m.phase = phase;
    return m.exhausted ? RawRoute::None : RawRoute::Pass;
}

void Machine::CopyRaw(ID3D12GraphicsCommandList *cmd, const FrameInputs &in, ID3D12Resource *target,
                      D3D12_RESOURCE_STATES state, std::uint32_t dstX, std::uint32_t dstY, UINT outputSubresource)
{
    const Resources &m = *res_;
    BarrierExternal(cmd, in.color, in.hostInputState, D3D12_RESOURCE_STATE_COPY_SOURCE, in.colorSubresource);
    BarrierExternal(cmd, target, state, D3D12_RESOURCE_STATE_COPY_DEST, outputSubresource);
    D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
    dst.pResource = target;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = outputSubresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES ? 0 : outputSubresource;
    src.pResource = in.color;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    const D3D12_BOX box{in.colorRect.x, in.colorRect.y, 0, in.colorRect.x + m.nativeW, in.colorRect.y + m.nativeH, 1};
    cmd->CopyTextureRegion(&dst, dstX, dstY, 0, &src, &box);
    BarrierExternal(cmd, target, D3D12_RESOURCE_STATE_COPY_DEST, state, outputSubresource);
    BarrierExternal(cmd, in.color, D3D12_RESOURCE_STATE_COPY_SOURCE, in.hostInputState, in.colorSubresource);
}

} // namespace ofps::core::temporal
