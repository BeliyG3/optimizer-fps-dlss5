#pragma once

#include "core/frame/feature_state.h"
#include "core/frame/warp_recorder.h"
#include "core/temporal/async_scheduler.h"
#include "core/frame/common.h"
#include "core/temporal/machine.h"

namespace ofps::core {
using gpu::kAsyncKickTag;

// POD frame data crosses the structured-exception guard.
struct AsyncCtx {
    FeatureState *st;
    ID3D12GraphicsCommandList *cmd;
    OfpsModelInputs inputs;
    OfpsFrameInputs frame;
    ID3D12Resource *color, *depth, *motion, *output;
    D3D12_RESOURCE_STATES depthState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    UINT depthSub = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    OfpsRect colorRect, depthRect, motionRect, outputRect;
    float mvScaleX, mvScaleY;
    bool depthInverted;
    temporal::FrameInputs tin;
    bool hostReset;
    const EvalContext *host;
    bool useBase;
    volatile int stage;
    int result;
};

int SnapshotAsyncCodecBase(FeatureState &st, ID3D12GraphicsCommandList *cmd, const OfpsResource &source, int slot);
int AsyncGuarded(AsyncCtx &c);
// async_host_sync.cpp
void AsyncCopyInput(ID3D12GraphicsCommandList *cmd, ID3D12Resource *host, ID3D12Resource *bg,
                    D3D12_RESOURCE_STATES &bgState, D3D12_RESOURCE_STATES hostState, UINT hostSub);
void AsyncQueueCap(FeatureState &st);
} // namespace ofps::core
