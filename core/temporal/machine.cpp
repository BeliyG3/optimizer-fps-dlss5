#include "core/temporal/machine.h"

#include "core/temporal/resources.h"

#include <algorithm>
#include <cstring>

namespace ofps::core::temporal {
namespace {

using ofps::core::gpu::Barrier;
using ofps::core::gpu::BarrierExternal;

constexpr DXGI_FORMAT kResidualFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
constexpr DXGI_FORMAT kChainFormat = DXGI_FORMAT_R16G16_FLOAT;

} // namespace

Machine::Machine() = default;

Machine::~Machine()
{
    delete res_;
}

bool Machine::Initialize(ID3D12Device *device, const ofps::core::gpu::Shaders &shaders, std::uint32_t nativeWidth, std::uint32_t nativeHeight,
                         DXGI_FORMAT outputFormat, DXGI_FORMAT outputView, std::uint32_t motionWidth, std::uint32_t motionHeight,
                         DXGI_FORMAT depthFormat, std::uint32_t depthWidth, std::uint32_t depthHeight, char *error, std::size_t errorSize,
                         std::uint32_t pictureDivisor)
{
    delete res_;
    res_ = nullptr;
    device_ = nullptr;
    Invalidate();
    if (!shaders.TemporalLoaded() || shaders.vertex.empty()) {
        if (error && errorSize) std::snprintf(error, errorSize, "temporal: shaders missing (optimizer-fps-dlss5\\temporal_*.dxbc)");
        return false;
    }
    auto resources = new Resources;
    if (!resources->Create(device, shaders, nativeWidth, nativeHeight, outputFormat, outputView, motionWidth, motionHeight,
                           depthFormat, depthWidth, depthHeight, error, errorSize, pictureDivisor)) {
        delete resources;
        return false;
    }
    res_ = resources;
    device_ = device;
    return true;
}

bool Machine::Matches(std::uint32_t nativeWidth, std::uint32_t nativeHeight, DXGI_FORMAT outputFormat, std::uint32_t motionWidth,
                      std::uint32_t motionHeight, DXGI_FORMAT depthFormat, std::uint32_t depthWidth, std::uint32_t depthHeight,
                      std::uint32_t pictureDivisor) const
{
    return res_ != nullptr && res_->nativeW == nativeWidth && res_->nativeH == nativeHeight && res_->outputFormat == outputFormat &&
           res_->motionW == motionWidth && res_->motionH == motionHeight && res_->depthFormat == depthFormat &&
           res_->depthW == depthWidth && res_->depthH == depthHeight && res_->historyPictureDivisor == pictureDivisor;
}

void Machine::SetUsePoint(const OfpsFencePoint &point, std::uint64_t evalNow, bool timing)
{
    if (res_) { res_->usePoint = point; res_->evalNow = evalNow; res_->timer.Enable(timing); }
}

bool Machine::TakeExhausted()
{
    if (!res_ || !res_->exhausted) return false;
    res_->exhausted = false;
    return true;
}

StatsSnapshot Machine::Stats() const {
    return res_ ? res_->stats.Snapshot() : StatsSnapshot{};
}

PassTimingSnapshot Machine::Timing() const {
    return res_ ? res_->timer.Snapshot() : PassTimingSnapshot{};
}

void Machine::Invalidate()
{
    flow_.Reset();
    flowRunning_ = false;
    accValid_ = false;
    pendingValid_ = false;
    pendingMirror_ = false;
    hasResidual_ = false;
    expectValid_ = false;
    expectPendingValid_ = false;
    if (res_) {
        res_->residualEverWritten = false; // nothing from before the reset is blended in
        res_->history.Reset();
        res_->phase.Start(0, false);
    }
}

void Machine::PromotePending()
{
    Resources &m = *res_;
    std::swap(m.acc[0], m.accP[0]); std::swap(m.acc[1], m.accP[1]);
    std::swap(m.accState[0], m.accPState[0]); std::swap(m.accState[1], m.accPState[1]);
    std::swap(m.accCurrent, m.accPCurrent);
    std::swap(m.accTarget[0], m.accPTarget[0]); std::swap(m.accTarget[1], m.accPTarget[1]);
    std::swap(m.expect[0], m.expectP[0]); std::swap(m.expect[1], m.expectP[1]);
    std::swap(m.expectState[0], m.expectPState[0]); std::swap(m.expectState[1], m.expectPState[1]);
    std::swap(m.expectTarget[0], m.expectPTarget[0]); std::swap(m.expectTarget[1], m.expectPTarget[1]);
    std::swap(m.expectCurrent, m.expectPCurrent);
    expectValid_ = expectPendingValid_;
    expectPendingValid_ = false;
    accValid_ = pendingValid_;
    pendingValid_ = false;
    pendingMirror_ = accValid_;
}

ID3D12Resource *Machine::Acc() const { return res_ ? res_->acc[res_->accCurrent] : nullptr; }

ID3D12Resource *Machine::NextAcc() const { return res_ ? res_->acc[1 - res_->accCurrent] : nullptr; }

ID3D12Resource *Machine::ModelMv() const { return (res_ && res_->modelMotionPso) ? res_->modelMv : nullptr; }

bool Machine::PhaseInActive() const { return res_ != nullptr && res_->applyPso != nullptr && res_->phase.Share() < 1.0f; }

void Machine::RecordCopyAcc(ID3D12GraphicsCommandList *cmd, bool pending, ID3D12Resource *dst, D3D12_RESOURCE_STATES dstState)
{
    Resources &m = *res_;
    ID3D12Resource *src = pending ? m.accP[m.accPCurrent] : m.acc[m.accCurrent];
    D3D12_RESOURCE_STATES &srcState = pending ? m.accPState[m.accPCurrent] : m.accState[m.accCurrent];
    Barrier(cmd, src, srcState, D3D12_RESOURCE_STATE_COPY_SOURCE);
    BarrierExternal(cmd, dst, dstState, D3D12_RESOURCE_STATE_COPY_DEST);
    cmd->CopyResource(dst, src);
    BarrierExternal(cmd, dst, D3D12_RESOURCE_STATE_COPY_DEST, dstState);
    Barrier(cmd, src, srcState, kAccState);
}

void Machine::CopyInterp(ID3D12GraphicsCommandList *cmd, ID3D12Resource *hostOutput, D3D12_RESOURCE_STATES hostOutputState,
                         std::uint32_t dstX, std::uint32_t dstY, UINT outputSubresource)
{
    Resources &m = *res_;
    Barrier(cmd, m.interp, m.interpState, D3D12_RESOURCE_STATE_COPY_SOURCE);
    BarrierExternal(cmd, hostOutput, hostOutputState, D3D12_RESOURCE_STATE_COPY_DEST, outputSubresource);
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = hostOutput;
    dst.SubresourceIndex = outputSubresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES ? 0 : outputSubresource;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = m.interp;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    const D3D12_BOX box{0, 0, 0, m.nativeW, m.nativeH, 1};
    cmd->CopyTextureRegion(&dst, dstX, dstY, 0, &src, &box);
    BarrierExternal(cmd, hostOutput, D3D12_RESOURCE_STATE_COPY_DEST, hostOutputState, outputSubresource);
}

void Machine::RecordReproject(ID3D12GraphicsCommandList *cmd, const FrameInputs &rawIn, ID3D12Resource *hostOutput,
                              D3D12_RESOURCE_STATES hostOutputState, std::uint32_t dstX, std::uint32_t dstY, UINT outputSubresource)
{
    Resources &m = *res_;
    const FrameInputs in = m.Resolve(rawIn);
    m.phase.Reproject();
    BarrierExternal(cmd, in.color, in.colorState, kReadable, in.colorSubresource);
    BarrierExternal(cmd, in.depth, in.depthState, kReadable, in.depthSubresource);
    Barrier(cmd, m.acc[m.accCurrent], m.accState[m.accCurrent], kAccState);
    Barrier(cmd, m.residual, m.residualState, kReadable);
    Barrier(cmd, m.depthF, m.depthFState, kReadable);
    Barrier(cmd, m.interp, m.interpState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    SlotKey key = m.BaseKey(in, m.acc[m.accCurrent]);
    key.res[1] = m.residual; key.fmt[1] = kResidualFormat; // t1 must be bound to something (the reprojection does not read it)
    const bool ramp = m.residualOld != nullptr && m.phase.Share() < 1.0f;
    if (ramp) {
        Barrier(cmd, m.residualOld, m.residualOldState, kReadable);
        key.res[10] = m.residualOld; key.fmt[10] = kResidualFormat;
    }
    if (m.residualLow) Barrier(cmd, m.residualLow, m.residualLowState, kReadable);
    if (m.colorF) Barrier(cmd, m.colorF, m.colorFState, kReadable);
    Constants c = m.BaseConstants(in);
    if (m.colorF == nullptr) c.tune[0] = 0.0f; // no snapshot: no colour test
    c.params[1] = in.residualCatmullRom ? 1.0f : 0.0f; // the reprojection's residual filter (PSAccumulate uses params.y for the chain validity)
    c.params[2] = static_cast<float>(in.debugVis);
    c.tune[3] = in.fillFloor ? 1.0f : 0.0f;
    m.history.Bind(m, cmd, key, c, in.history ? in.historyPasses : 0u);
    if (!ramp) c.smoothing[3] = 1.0f; // no phase-in this frame: the stored residual alone
    // 26.6.X: the reprojection also writes its addition + acceptance; the compose pass then smooths the
    // addition along the original and writes the frame.
    const bool compose = in.compose && m.composePso != nullptr && m.toneAcc != nullptr && in.smoothRadius > 0.0f && in.debugVis == 0 && !in.rawInterpolation;
    if (compose) {
        Barrier(cmd, m.toneAcc, m.toneAccState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        const Resources::Output additionOutput = m.Target(kTargetAddition);
        m.Dispatch(cmd, kReproject, m.Target(kTargetInterp), m.nativeW, m.nativeH, key, c, &additionOutput);
        Barrier(cmd, m.toneAcc, m.toneAccState, kReadable);
        // 26.28 PW_T_CELLS: what the reprojection accepted, averaged per cell of the low-res grid with
        // the mean look of the pixels it came from. A rejected pixel then takes a smooth blend of the
        // cells around it instead of deciding for itself from its own taps (which dithered).
        const bool cells = m.cellsPso != nullptr && m.cellsAdd != nullptr && in.cells;
        if (cells) {
            SlotKey ak;
            ak.valid = true;
            ak.res[0] = in.color; ak.fmt[0] = in.colorView;
            ak.res[2] = m.toneAcc; ak.fmt[2] = kResidualFormat;
            Barrier(cmd, m.cellsAdd, m.cellsAddState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Barrier(cmd, m.cellsLook, m.cellsLookState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            const Resources::Output lookTarget = m.Target(kTargetCellsLook);
            m.Dispatch(cmd, kCells, m.Target(kTargetCellsAdd), m.lowW, m.lowH, ak, c, &lookTarget);
            Barrier(cmd, m.cellsAdd, m.cellsAddState, kReadable);
            Barrier(cmd, m.cellsLook, m.cellsLookState, kReadable);
        }
        SlotKey ck;
        ck.valid = true;
        ck.res[0] = in.color; ck.fmt[0] = in.colorView;
        ck.res[2] = m.toneAcc; ck.fmt[2] = kResidualFormat;
        ck.res[5] = in.depth; ck.fmt[5] = in.depthView;
        if (cells) {
            ck.res[1] = m.cellsLook; ck.fmt[1] = kResidualFormat;
            ck.res[8] = m.cellsAdd; ck.fmt[8] = kResidualFormat;
        }
        Constants cc = c;
        cc.fill[0] = cells ? 1.0f : 0.0f; // PSCompose reads PwTFill.x as "the cells are bound"
        m.Dispatch(cmd, kCompose, m.Target(kTargetInterp), m.nativeW, m.nativeH, ck, cc);
    } else {
        m.Dispatch(cmd, kReproject, m.Target(kTargetInterp), m.nativeW, m.nativeH, key, c);
    }
    if (m.statsPso && !m.exhausted)
        m.stats.Record(m, cmd, key, c, in.fillFloor);
    BarrierExternal(cmd, in.color, kReadable, in.colorState, in.colorSubresource);
    BarrierExternal(cmd, in.depth, kReadable, in.depthState, in.depthSubresource);
    if (m.exhausted || hostOutput == nullptr) return; // diagnostics: the target is kept, the host's Output is left alone
    CopyInterp(cmd, hostOutput, hostOutputState, dstX, dstY, outputSubresource);
}

void Machine::RecordRaw(ID3D12GraphicsCommandList *cmd, const FrameInputs &in, ID3D12Resource *target,
                        D3D12_RESOURCE_STATES state, std::uint32_t dstX, std::uint32_t dstY, UINT outputSubresource)
{
    const auto phase = res_->phase;
    FrameInputs raw = in;
    raw.base = nullptr;
    raw.debugVis = 3;
    raw.phaseInFrames = 0;
    RecordReproject(cmd, raw, target, state, dstX, dstY, outputSubresource);
    res_->phase = phase;
}

void Machine::RecordDebugCopies(ID3D12GraphicsCommandList *cmd, const FrameInputs &rawIn, ID3D12Resource *readback, std::uint32_t x,
                                std::uint32_t y)
{
    Resources &m = *res_;
    const FrameInputs in = m.Resolve(rawIn);
    auto copyRow = [&](ID3D12Resource *src, DXGI_FORMAT format, std::uint32_t texelBytes, std::uint32_t sx,
                       std::uint32_t sy, UINT64 offset,
                       UINT subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES) {
        const std::uint32_t texels = 256 / texelBytes;
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = readback;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Offset = offset;
        dst.PlacedFootprint.Footprint.Format = format;
        dst.PlacedFootprint.Footprint.Width = texels;
        dst.PlacedFootprint.Footprint.Height = 1;
        dst.PlacedFootprint.Footprint.Depth = 1;
        dst.PlacedFootprint.Footprint.RowPitch = 256;
        D3D12_TEXTURE_COPY_LOCATION s{};
        s.SubresourceIndex = subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES ? 0 : subresource;
        s.pResource = src;
        s.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        const D3D12_BOX box{sx, sy, 0, sx + texels, sy + 1, 1};
        cmd->CopyTextureRegion(&dst, 0, 0, 0, &s, &box);
    };
    BarrierExternal(cmd, in.color, in.colorState, D3D12_RESOURCE_STATE_COPY_SOURCE, in.colorSubresource);
    copyRow(in.color, in.colorView, 8, in.colorRect.x + x, in.colorRect.y + y, 0, in.colorSubresource);
    BarrierExternal(cmd, in.color, D3D12_RESOURCE_STATE_COPY_SOURCE, in.colorState, in.colorSubresource);
    Barrier(cmd, m.residual, m.residualState, D3D12_RESOURCE_STATE_COPY_SOURCE);
    copyRow(m.residual, kResidualFormat, 8, x, y, 256);
    Barrier(cmd, m.residual, m.residualState, kReadable);
    Barrier(cmd, m.interp, m.interpState, D3D12_RESOURCE_STATE_COPY_SOURCE);
    copyRow(m.interp, m.outputView, 8, x, y, 512);
    Barrier(cmd, m.acc[m.accCurrent], m.accState[m.accCurrent], D3D12_RESOURCE_STATE_COPY_SOURCE);
    copyRow(m.acc[m.accCurrent], kChainFormat, 4, in.motionRect.x + x * in.motionRect.w / m.nativeW,
            in.motionRect.y + y * in.motionRect.h / m.nativeH, 768);
    Barrier(cmd, m.acc[m.accCurrent], m.accState[m.accCurrent], kAccState);
}

} // namespace ofps::core::temporal
