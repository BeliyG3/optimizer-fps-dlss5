#pragma once

// NGX Feature-18 interposer.
//
// Some DLSS Neural Rendering hosts (RenoDX "DLSS 5 Neural Rendering") drive nvngx_dlssnr.dll
// directly: they resolve its exports and call NVSDK_NGX_D3D12_CreateFeature / EvaluateFeature for
// feature 18 on the game's command list. This unit hooks those exports in memory (Detours) and, when
// the add-on's layout is not Off, packs the host's colour/depth/motion into the work frame, runs the
// model on the packed frame, then unpacks the model output back into the host's native output. The
// host keeps its own colour codec, guides and state; only the frame the model sees changes.

#include "diagnostics.h"
#include "peripheral_warp/types_v2.h"

#include <cstdint>

struct ID3D12Device;
struct ID3D12CommandQueue;
struct ID3D12GraphicsCommandList;

namespace pw_ngx {

struct Status {
    bool moduleFound = false;
    bool hooked = false;
    bool featureCreated = false;
    bool adopted = false;            // the feature was created before the hook and taken over at its first evaluate
    bool active = false;             // the last evaluate went through Pack -> NR -> Unpack
    std::uint32_t hookAttempts = 0;  // Poll() attempts to install the hooks (Detours can fail transiently)
    std::uint32_t nativeWidth = 0;
    std::uint32_t nativeHeight = 0;
    std::uint32_t workWidth = 0;
    std::uint32_t workHeight = 0;
    std::uint64_t evaluations = 0;   // evaluates that went through the warp
    std::uint64_t passthroughs = 0;  // evaluates forwarded untouched
    int lastNgxResult = 0;
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

// Temporal NR modes (stage 24). 0: the model runs every frame. 1 Interpolate: a full model pass
// every `every`-th frame, the frames between receive the current colour plus the last pass's
// residual reprojected along the host's motion vectors. 3: the same, with the model's pass running
// on a background queue. (Mode 2, "centre every frame", was withdrawn in 26.26; the value is still
// accepted on the ABI and mapped to 1.)
struct TemporalSettings {
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

using GetConfigFn = pw::ConfigV2 (*)();
using LogFn = void (*)(bool warning, const char *message);

// Called once from the add-on; the config getter is polled on every create/evaluate.
void Configure(GetConfigFn getConfig, LogFn log, const wchar_t *shaderDirectory);
// Hosts that apply the warp themselves (OptiScaler through the layout bridge) must not be hooked on
// top; the add-on disables the interposer while such a bridge is linked. Default: enabled.
void SetEnabled(bool enabled);
// 26.16 crash guard: while set, every feature-18 call is forwarded untouched (no state, no adoption, no warp).
void SetSafeMode(bool safe);
bool SafeMode();
// 26.21 crash guard: called once, on the render thread, just before the first warped evaluate is
// recorded - so the crash marker exists even when that very evaluate kills the process (the marker
// used to be written from the next present, which such a crash never reaches).
using FirstWarpedFn = void (*)();
void SetFirstWarpedCallback(FirstWarpedFn callback);
// Call from a present callback: finds nvngx_dlssnr.dll once it is loaded and installs the hooks.
void Poll();
// Which diagnostic outlines are drawn into the unpacked output (Peripheral mode only, as the
// SDK's shader draws them): the cyan Center boundary and the orange raw-Work boundary.
void SetOutlines(bool center, bool work);
// Adjustment applied on top of the host's motion scale before Pack: a multiplier and a sign flip.
// Defaults (1, false) leave the host's values untouched.
void SetMotionAdjust(float scale, bool invert);
// Colour adjustment of the unpacked frame (warped path only): brightness in percent (0 = none)
// and gamma (1 = none), applied as (1 + brightness/100) * pow(rgb, 1/gamma).
void SetOutputColorAdjust(float brightnessPercent, float gamma);
void SetTemporal(const TemporalSettings &settings);
// Background mode: the host submitted `list` on `queue` (ReShade execute_command_list); lets the
// background queue start the pass whose input copies were recorded on that list.
void OnCommandListExecuted(ID3D12CommandQueue *queue, ID3D12GraphicsCommandList *list);
Status GetStatus();

// The D3D12 graphics queues that execute the host's command lists (the add-on learns them from
// ReShade's init/destroy_command_queue events). Before the interposer releases anything the GPU may
// still be reading (its packed slots, the model, the descriptor heaps) it signals a fence on these
// queues and waits: the host's queue runs several frames behind the CPU, and freeing an object in
// flight removes the device. Without a registered queue, releases are deferred by a number of
// evaluates instead. Both pointers are the native objects, not ReShade's proxies.
void RegisterQueue(ID3D12Device *device, ID3D12CommandQueue *queue);
void UnregisterQueue(ID3D12CommandQueue *queue);

} // namespace pw_ngx
