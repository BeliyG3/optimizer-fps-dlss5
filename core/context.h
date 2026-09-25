#pragma once
#include "core/temporal/diagnostic_keys.h"
#include "core/temporal/profile.h"

// The interposer's process-wide state (stage 27.D1). Everything that used to be a file-scope `g_*`
// of ngx_hook.cpp lives in one CoreContext, reached through Ctx(); the modules address it by name.
// The context is a function-local static, so no module depends on another one's initialization
// order.

#include "core/frame/feature_state.h"
#include "core/gpu/submission.h"
#include "core/frame/common.h"

#include "optimizer_fps/types_v2.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace ofps::core {
using EventSink = void (*)(OfpsEvent, const OfpsEventData &);
void SetEventSink(EventSink sink);
void EmitEvent(OfpsEvent event, const OfpsEventData &data);

struct Status {
    bool moduleFound = false;
    bool hooked = false;
    bool featureCreated = false;
    bool adopted = false;            // the feature was created before the hook and taken over at its first evaluate
    bool active = false;             // the last evaluate went through Pack -> NR -> Unpack
    std::uint32_t warpPath = OFPS_WARP_NONE;
    char warpPathReason[192] = {};
    std::uint32_t hookAttempts = 0;  // Poll() attempts to install the hooks (Detours can fail transiently)
    std::uint32_t nativeWidth = 0;
    std::uint32_t nativeHeight = 0;
    std::uint32_t workWidth = 0;
    std::uint32_t workHeight = 0;
    std::uint64_t submissionDrops = 0; // Ring overflow diagnostics; does not disable features.
    std::uint64_t evaluations = 0;   // evaluates that went through the warp
    std::uint64_t passthroughs = 0;  // evaluates forwarded untouched
    int lastNgxResult = 0;
    int modelPassesRunning = 1;
    std::uint64_t modelPassEvaluations[3] = {};
    char modelPassReason[192] = {};
    char reason[512] = {};
    // What the host hands the model for motion (the last input set seen): texture size, subrect,
    // the MVecScale it wrote and whether that value could be read from the parameter block.
    std::uint32_t hostMotionWidth = 0, hostMotionHeight = 0;
    std::uint32_t hostMotionRectWidth = 0, hostMotionRectHeight = 0;
    float hostMotionScaleX = 0.0f, hostMotionScaleY = 0.0f;
    bool hostMotionScaleRead = false;
    int floatGetterSlot = -1;        // parameter-block vtable slot that answered float reads (-1: none)
    // [PeripheralWarp] DebugTiming=1: GPU time of the model's evaluate (timestamp queries); 0 when timing is off.
    float modelMsFull = 0.0f;
    std::uint32_t modelSamplesFull = 0;
    // Temporal modes: the mode in effect, frames the model ran on (full or centre) and frames that
    // were interpolated from the last residual; `temporalReason` says why the mode is not applied.
    int temporalMode = 0;
    std::uint64_t fullFrames = 0, interpFrames = 0;
    char temporalReason[160] = {};
    bool temporalFlowRequested = false, temporalFlowRunning = false;
    char temporalFlowReason[160] = {};
    ofps::core::temporal::StatsSnapshot temporalStats{};
    bool temporalGuideProbes = false;
    ofps::core::temporal::PassTimingSnapshot temporalPassTiming{};
    bool temporalTimingEnabled = false;
    // Background mode (3): passes the model completed on its own queue, their rate, the age of the
    // residual on screen (frames), forced queue waits (age limit), CPU stalls (allocator reuse), the
    // model's GPU time on the background queue and that queue's type.
    std::uint64_t asyncPasses = 0, asyncForcedWaits = 0, asyncStalls = 0;
    float asyncPassesPerSecond = 0.0f, asyncModelMs = 0.0f;
    std::uint32_t asyncAge = 0;
    char asyncQueue[8] = {};
    std::uint64_t asyncQueueWaits = 0; // evaluates that waited for the GPU queue cap
    float asyncQueueWaitMs = 0.0f;     // smoothed CPU wait per evaluate for the cap
    // Frames that no path produced (model or a stage failed, exception, no model yet): the host's
    // colour was written to its output instead, so the frame is never left unwritten; the last reason.
    std::uint64_t fallbackFrames = 0;
    char fallbackReason[128] = {};
};

struct TemporalSettings {
    int modelPasses = 1;
    bool spreadPasses = true;
    int mode = 0;                  // 0 every frame, 1 Interpolate (sync), 2 withdrawn (maps to 1), 3 Interpolate with the model in the background
    int every = 2;                 // 1..4 (mode 1: 2..4); mode 3: frames given to one background pass (1 = continuous)
    int maxAge = 8;                // mode 3: residual age (frames) after which the host queue waits for the background pass
    int maxQueue = 2;              // mode 3: GPU frames allowed unfinished when a frame is recorded (0 = off): the CPU waits, so the kick's copies reach the GPU sooner
    // Reprojection validity: relative depth mismatch against the residual frame's depth that counts
    // as a disocclusion (0 = off; 5 % - 2 % also rejected sloped surfaces sampled a texel off) and the
    // relative luma mismatch against the residual frame's colour (0 = off).
    float depthTolerance = 0.05f;
    float colorTolerance = 0.08f;
    // Rejected pixels (disocclusions, frame edge) get the box-filtered residual instead of nothing.
    bool holeFill = true;
    float mvSearchRadiusPx = 16.0f; // 26.14: search radius (native px) for a depth-matching motion texel when the pixel's own texels belong to another surface (block-constant optical-flow vectors); 0 = off
    bool residualCatmullRom = true; // interpolated frames resample the residual with Catmull-Rom instead of bilinear
    // Warped path: residual and interpolated frames are based on the packed colour unpacked without the
    // model (stage 26.3), so the residual carries only the model's contribution. Off = old behaviour.
    bool warpBase = true;
    // 26.6.X: on interpolated frames the reprojection's addition is smoothed along the original frame's own
    // smoothness (colour + depth) around rejected pixels, so no step appears where the game's frame has none.
    float smoothRadiusPx = 24.0f;  // 0 = off
    // Diagnostics (all default off): flip the motion vectors' sign for the temporal machine;
    // interpolated frames show the raw colour; the model gets single-frame vectors on full passes
    // (no accumulation); a status line in the log every 60 interpolated frames.
    bool flipMotionSign = false;
    bool debugRawInterpolation = false;
    int debugView = 0;             // 26.17 diagnostics: interpolated frames show 2 = what the model changed (residual x4 around grey), 3 = raw colour, 1 = displacement/acceptance
    bool debugSingleFrameMotion = false;
    bool debugLog = false;
};

struct DiagnosticsConfig {
    bool timing = false;              // DebugTiming: timestamp queries around the model's evaluate
    bool temporalReadback = false;    // DebugTemporalReadback: centre texels printed every 60th interpolated frame
    bool temporalKeepOutput = false;  // DebugTemporalKeepOutput: interpolated frames do not write the host Output
    float temporalBlend = -1.0f;      // DebugTemporalBlend: cross-pass residual weight (<0: the built-in 0.6)
    float temporalDepth = -1.0f;      // DebugTemporalDepth: depth tolerance override (<0: the setting's value)
    float temporalSmooth = -1.0f;     // DebugTemporalSmooth: compose radius override, 0 = off (<0: the setting's value)
    // 26.28: switches for the passes ported from the OptiScaler fork, so a regression can be bisected
    // in a game without a rebuild. Background defaults follow the uncontended GPU comparison.
    bool temporalNoExpect = false;      // DebugTemporalNoExpect: depth tests compare this frame's depth, as before 26.28
    bool temporalNoCells = false;       // DebugTemporalNoCells: rejected pixels are not painted from the accepted cells
    bool temporalNoModelMotion = false; // DebugTemporalNoModelMotion: the model gets the plain chain, not the tested one
    bool temporalNoBackgroundModelMotion = false; // Same key; validation improves the measured background combination
    int temporalPhaseIn = -1;           // DebugTemporalPhaseIn: carried frames a new pass fades in over (0 = off, <0 = automatic)
    int temporalBackgroundPhaseIn = -1; // Same key; automatic phase-in, evaluated on the D3D12 path-traced lab
    bool debugLayerLog = false;       // DebugLayer=1 (which also turns the D3D12 debug layer on): dump its messages
    bool asyncCompute = false;        // DebugAsyncCompute: background pass on a compute queue instead of a direct one
    bool asyncNormalPriority = false; // DebugAsyncNormalPriority: normal instead of high queue priority
    bool asyncNoRealtime = false;     // DebugAsyncNoRealtime: skip the GLOBAL_REALTIME request
    bool asyncLog = false;            // DebugAsyncLog: verbose background-pass log
    bool asyncShowPass = false;       // DebugAsyncShowPass: show the last pass's output as is (no reprojection)
    unsigned hookDelayMs = 0;         // DebugHookDelayMs: delay the hook so a host creates its model first
    bool keepBackbuffer = false;      // DebugKeepBackbuffer: hand the native UI/back buffer to the warped model
    int depthState = 99;              // DebugDepthState: host depth resting state (99 = follow the automatic rule)
    // DebugMotionSmooth: smooth the host's block-constant optical flow vectors before the model. Off by
    // default: on the DX9 bench it halved the measured lattice but turned the square blotches of the
    // fast optical flow into outlines - they are wrong vectors, which interpolation spreads rather than
    // fixes; the flow's own quality setting (ofa_perf=5) removes them instead.
    bool motionSmooth = false;
    float motionSmoothTolerance = -1.0f; // DebugMotionSmoothTolerance: relative depth mismatch that separates surfaces (<0: built in)
    // DebugPassNoHistory: the extra model passes (2 and 3) run with frame reset=1 on every evaluate, so
    // they keep no history of their own and never move it along the host's vectors; the first pass keeps
    // its history. Same number of evaluates, so the same cost. Tried against the square blotches of block
    // optical flow compounding over two passes: on the DX9 bench (Witcher EE settings, 2026-09-21) the
    // blotches stayed and the change between frames grew from 4.8 to 7-10 luma levels. Kept for A/B in
    // scenes the bench does not have.
    bool passNoHistory = false;
};

using GetConfigFn = ofps::sdk::ConfigV2 (*)();

using FirstWarpedFn = void (*)();


struct CoreContext {
    // ---- the real entry points of nvngx_dlssnr.dll (every call goes through the forwarder)

    GetConfigFn getConfig = nullptr;
    ofps::sdk::ConfigV2 config = ofps::sdk::DefaultConfigV2();
    OfpsSettingsValues values{};
    std::wstring shaderDirectory;
    // 26.26: the overlay writes these from the present/UI thread while the evaluate thread reads them.
    // Each is independent of the others (no invariant spans two), so a relaxed atomic is enough; the
    // settings that must move together (TemporalSettings) still go under the mutex in SetTemporal.
    std::atomic<bool> showCenterOutline{false};
    std::atomic<bool> showWorkOutline{false};
    std::atomic<float> motionScaleAdjust{1.0f};
    std::atomic<bool> motionInvert{false};
    // The 64-bit Feed host's optical flow grid when its vectors are block-constant cells, 0 otherwise
    // (pw_ngx::SetBlockMotionGrid). Above 1 the evaluate smooths them first (motion_smooth.h).
    std::atomic<int> blockMotionGrid{0};
    std::atomic<std::uint32_t> temporalMotionSource{0}; // private OptiScaler option, never a host motion grid
    std::atomic<float> outputGain{1.0f};
    std::atomic<float> outputGamma{1.0f};
    TemporalSettings temporal;
    // 26.26: the diagnostic switches, read once from the read-only [PeripheralWarp] Debug* keys at add-on
    // load (producer.cpp) and never written back. They used to be PW_NGX_* environment variables; no
    // environment variable is read any more. Set before the first evaluate, read without a lock after.
    DiagnosticsConfig diag;
    ofps::temporal::DiagnosticValuesV1 temporalDiagnostics{};
    ofps::temporal::TemporalProfile frameProfile{};

    std::atomic<bool> enabled{true};
    // 26.21: fired once, before the first warped evaluate is recorded, so the crash guard's marker is on
    // disk even if that evaluate never returns (the marker used to be written from the next present).
    std::atomic<FirstWarpedFn> firstWarpedCallback{nullptr};
    std::atomic<bool> firstWarpedFired{false};
    // 2026.10: the first device the warp ran on, referenced until the process ends, so the crash guard
    // can ask at exit whether it was removed (pw_ngx::WarpDeviceRemoved).
    std::atomic<ID3D12Device *> warpDevice{nullptr};
    std::atomic<bool> safeMode{false}; // 26.16: forward everything untouched (the previous session of this game died after our first warped frame)
    // 26.22: features of a second NR consumer are not ours at all. They used to get a FeatureState and a pass-through
    // model created through our forwarder; in Control that create crashed inside the model's own CreateFeature
    // (D3D12Core SetDescriptorHeaps on the caller's list) although the same call works without the hook. Now the
    // original entry points are called with the caller's arguments untouched and the handle is only remembered so
    // that its evaluates/releases pass straight through too.
    std::unordered_set<void *> foreign;
    gpu::Submission submission;
    std::mutex mutex;
    Status status;
    std::uint64_t evalCounter = 0; // every HookEvaluate, warped or not

    ofps::core::gpu::Shaders shaders;
    std::unordered_map<void *, std::unique_ptr<FeatureState>> features;
    ofps::core::gpu::Graveyard graveyard; // under the mutex
    std::atomic<bool> anyAsyncSignalPending{false}; // 26.7.4: the execute_command_list handler touches nothing unless a background kick awaits its submit
    ofps::core::gpu::CrashInfo crash;
    // [PeripheralWarp] DebugLayer=1: how many debug-layer messages have been printed so far (first 200).
    std::uint64_t debugLayerPrinted = 0;
};

CoreContext &Ctx();
extern thread_local bool insideCore;
// Declare after acquiring Ctx().mutex; also protects callbacks during housekeeping.
class InsideCoreScope {
public:
    InsideCoreScope() : previous_(insideCore) { insideCore = true; }
    ~InsideCoreScope() { insideCore = previous_; }
    InsideCoreScope(const InsideCoreScope &) = delete;
    InsideCoreScope &operator=(const InsideCoreScope &) = delete;
private:
    bool previous_;
};

// ---- calls into the snippet through the forwarder

bool KeepBackbufferEnabled(); // DebugKeepBackbuffer
void NotifyFirstWarped();
bool WaitForGpu(ID3D12Device *device, ID3D12Device *proxyDevice = nullptr);
// 26.18: releases and re-creations no longer stall the CPU. A fence is signalled on the host's queues and the
// objects go to the graveyard gated on it; they are dropped on a later evaluate once the GPU has passed the
// signal. The CPU wait is only reached when no queue could be signalled.
void SetReason(const char *fmt, ...);
bool LoadShaders();
void DrainGraveyard(bool everything);
void NoteCommandListExecuted(ID3D12CommandQueue *queue, ID3D12CommandList *list);
void DrainSubmissions(gpu::Submission::Signal policy);
void TemporalFlowNoteSubmissions(const gpu::SubmissionEntry *entries, std::uint32_t count);
void Housekeeping();
OfpsFencePoint HostUsePoint(ID3D12CommandList *cmd);

bool TimingEnabled(); // DebugTiming=1
// Experiment: DebugTemporalKeepOutput=1 leaves the host's Output untouched on interpolated frames
// (the reprojection is still recorded into the machine's target).
bool KeepOutputOnInterpolation();
float ResidualBlendWeight();
#define kResidualBlend ResidualBlendWeight()

} // namespace ofps::core
