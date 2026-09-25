#pragma once

// Shared frame/GPU declarations and model-owned resource states.

#include "core/gpu/queues.h"
#include "core/gpu/graveyard.h"
#include "core/gpu/shaders.h"
#include "core/gpu/barriers.h"
#include "core/gpu/crash_guard_seh.h"
#include "core/log.h"
#include "core/api/ofps_core.h"

#include <cstdint>

namespace ofps::core {

using gpu::TypedView;
using gpu::EncodingFor;
using gpu::Barrier;
using gpu::BarrierExternal;
using gpu::CreateTexture;
using gpu::kDeferredReleaseEvaluates;

// Background model pass (temporal mode 3): the model runs on its own queue on private copies of the
// host's inputs, one pass in flight at a time; the host's frames only reproject the last result.
bool AsyncVerbose();

// Private model inputs and outputs have fixed resting states; host states come from the frame.
constexpr D3D12_RESOURCE_STATES kModelInputState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
constexpr D3D12_RESOURCE_STATES kModelOutputState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

constexpr int kCrashed = -1000001;
constexpr int kPackFailed = -1000002;
// Returned by the temporal paths that decline a frame, so the dispatch falls back to the plain
// pass-through.
constexpr int kNotHandled = -1000003;

enum Stage : int {
    StagePackBarriers = 1,
    StagePackDraw,
    StagePackRestore,
    StageParamsWrite,
    StageModel,
    StageUnpackDescriptors,
    StageUnpackDraw,
    StageCopyOut,
    StageParamsRestore,
    StageDone,
    StageTemporalAccumulate,
    StageTemporalResidual,
    StageTemporalReproject,
};

inline const char *StageName(int stage)
{
    switch (stage) {
    case StagePackBarriers: return "pack barriers";
    case StagePackDraw: return "pack draw";
    case StagePackRestore: return "pack restore barriers";
    case StageParamsWrite: return "writing model parameters";
    case StageModel: return "model evaluate";
    case StageUnpackDescriptors: return "unpack descriptors";
    case StageUnpackDraw: return "unpack draw";
    case StageCopyOut: return "copy to host output";
    case StageParamsRestore: return "restoring parameters";
    case StageDone: return "done";
    case StageTemporalAccumulate: return "temporal: motion accumulation";
    case StageTemporalResidual: return "temporal: residual";
    case StageTemporalReproject: return "temporal: reprojection";
    default: return "before pack";
    }
}

} // namespace ofps::core
