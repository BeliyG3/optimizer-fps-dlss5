#pragma once

// Declarations every module of the feature-18 hook shares (stage 27.D1): the parameter-block
// aliases, the real entry-point signatures, the host's resource states, the stage codes of the
// guarded bodies and the small result codes the dispatch layer answers with.

#include "ngx_common.h"

#include <cstdint>

namespace pwhook {

// Parameter-block access, forwarder, queues, graveyard, shaders and format helpers live in
// ngx_common (shared with the DLSS interposer).
using pwngx::kNgxSuccess;
using pwngx::SetUInt;
using pwngx::GetUInt;
using pwngx::SetFloat;
using pwngx::GetFloat;
using pwngx::ProbeKey;
using pwngx::TypedView;
using pwngx::EncodingFor;
using pwngx::Barrier;
using pwngx::BarrierExternal;
using pwngx::CreateTexture;
constexpr int kFeatureNeuralRendering = 18;

inline void SetResource(void *p, const char *name, ID3D12Resource *v) { pwngx::SetPointer(p, name, v); }
inline ID3D12Resource *GetResource(void *p, const char *name) { return static_cast<ID3D12Resource *>(pwngx::GetPointer(p, name)); }

// ---------------------------------------------------------------------------------------------
// Snippet exports (real entry points; every call goes through the forwarder, see ngx_common).
// ---------------------------------------------------------------------------------------------
using PFN_Create = int(__cdecl *)(ID3D12GraphicsCommandList *, int, void *, void **);
using PFN_Evaluate = int(__cdecl *)(ID3D12GraphicsCommandList *, void *, void *, void *);
using PFN_Release = int(__cdecl *)(void *);

using pwngx::kDeferredReleaseEvaluates;
using pwngx::Log;

struct Subrect {
    unsigned int x = 0, y = 0, w = 0, h = 0;
    bool present = false;
};

// Background model pass (temporal mode 3): the model runs on its own queue on private copies of the
// host's inputs, one pass in flight at a time; the host's frames only reproject the last result.
bool AsyncVerbose();

// The host's input resources arrive in the state NGX expects for feature inputs (shader-readable);
// its output in the state NGX writes it in (unordered access). Pack samples the inputs from a pixel
// shader, so they are moved to PIXEL_SHADER_RESOURCE and back around the draw.
constexpr D3D12_RESOURCE_STATES kHostInputState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
constexpr D3D12_RESOURCE_STATES kHostOutputState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

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

} // namespace pwhook
