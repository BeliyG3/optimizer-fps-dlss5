#include "core/temporal/machine.h"
#include "core/temporal/resources.h"
#include "core/context.h"
#include "core/log.h"

namespace ofps::core::temporal {
using gpu::Barrier;
using gpu::BarrierExternal;

void Machine::RecordFlowCapture(ID3D12GraphicsCommandList *cmd, const FrameInputs &rawIn,
                                ID3D12Device *nativeDevice, bool full, bool referenceOnly) {
    if (!rawIn.opticalFlow || !cmd || !res_) return;
    if (full && !referenceOnly) flowRunning_ = false;
    Resources &m = *res_;
    if (!flow_.Acquire(nativeDevice, m.nativeW / 2, m.nativeH / 2)) {
        if (!flowLogged_) Log(true, "Optimizer FPS optical flow unavailable: %s", flow_.Problem().c_str());
        flowLogged_ = true;
        return;
    }
    if (!flowLogged_) Log(false, "Optimizer FPS optical flow: temporal session %ux%u, grid %u",
                          flow_.Width(), flow_.Height(), flow_.Grid());
    flowLogged_ = true;
    const FrameInputs in = m.Resolve(rawIn);
    BarrierExternal(cmd, in.color, in.colorState, kReadable, in.colorSubresource);
    SlotKey key = m.BaseKey(in, m.acc[m.accCurrent]);
    const Constants constants = m.BaseConstants(in);
    const auto capture = [&](ID3D12Resource *image) {
        BarrierExternal(cmd, image, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        m.Dispatch(cmd, kFlowLuma, {image, DXGI_FORMAT_R8_UNORM}, flow_.Width(), flow_.Height(),
                   key, constants);
        BarrierExternal(cmd, image, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
        return !m.exhausted;
    };
    if (!referenceOnly) flow_.NowWritten(capture(flow_.Now()));
    if (full) flow_.ThenWritten(capture(flow_.Then()));
    BarrierExternal(cmd, in.color, kReadable, in.colorState, in.colorSubresource);
    if (!referenceOnly && !flow_.Arm(cmd) && flow_.Problem().empty())
        flow_.Failed("optical flow capture is incomplete");
}

bool Machine::RecordFlowSeed(ID3D12GraphicsCommandList *cmd, const FrameInputs &in) {
    flowRunning_ = false;
    if (!in.opticalFlow || !flow_.HasSession()) return false;
    if (!flow_.Ready()) {
        if (flow_.Problem().empty()) flow_.Failed("previous optical flow frame has no registered submission queue");
        return false;
    }
    const bool executed = flow_.Execute();
    flow_.Consumed();
    if (!executed) {
        flow_.Disable("optical flow fence timed out or execute failed; using game vectors");
        Log(true, "Optimizer FPS optical flow unavailable: %s", flow_.Problem().c_str());
        return false;
    }
    Resources &m = *res_;
    BarrierExternal(cmd, flow_.Field(), D3D12_RESOURCE_STATE_COMMON, kReadable);
    const int seeded = 1 - m.accCurrent;
    Barrier(cmd, m.acc[seeded], m.accState[seeded], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    SlotKey key{};
    key.valid = true;
    key.res[9] = flow_.Field();
    key.fmt[9] = DXGI_FORMAT_R16G16_SINT;
    m.Dispatch(cmd, kFlowChain, m.Target(m.accTarget[seeded]), m.motionW, m.motionH, key,
               m.BaseConstants(m.Resolve(in)), nullptr,
               {flow_.Width(), flow_.Height(), flow_.Grid()});
    Barrier(cmd, m.acc[seeded], m.accState[seeded], kAccState);
    BarrierExternal(cmd, flow_.Field(), kReadable, D3D12_RESOURCE_STATE_COMMON);
    if (m.exhausted) return false;
    m.accCurrent = seeded;
    accValid_ = true;
    flowRunning_ = true;
    flow_.Succeeded();
    if (!flowActiveLogged_) Log(false, "Optimizer FPS optical flow: hybrid field active");
    flowActiveLogged_ = true;
    return true;
}
} // namespace ofps::core::temporal

namespace ofps::core {
void TemporalFlowNoteSubmissions(const gpu::SubmissionEntry *entries, std::uint32_t count) {
    for (std::uint32_t i = 0; i < count; ++i) {
        if (!entries[i].flowTag) continue;
        ID3D12CommandQueue *queue = gpu::RetainRegisteredQueue(entries[i].queue);
        if (!queue) continue;
        for (auto &feature : Ctx().features)
            if (feature.second->temporal) feature.second->temporal->NoteFlowSubmission(entries[i].flowTag, queue);
        queue->Release();
    }
}
} // namespace ofps::core
