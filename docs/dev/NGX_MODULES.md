# The NGX feature-18 hook, module by module

Stage 27.D1 split `adapters/reshade/ngx_hook.cpp` (~2 900 lines, one translation unit of `g_*`
globals and free functions) into `adapters/reshade/ngx/`. Nothing changed but where the code
lives: the public include stays `adapters/reshade/ngx_hook.h`, and everything internal lives in
one namespace, `pwhook`, so the modules can see each other without a global.

## `hook_common.h`

The vocabulary every module shares: the `pwngx::` parameter-block aliases (`GetUInt`, `SetFloat`,
`Barrier`, `TypedView`, `Log`, …), the resource pointer helpers over the block, the signatures of
the three real `nvngx_dlssnr.dll` entry points, `Subrect`, the resting states the host hands its
inputs and output in (`kHostInputState`, `kHostOutputState`), the result codes the internal paths
answer with (`kCrashed`, `kPackFailed`, `kNotHandled`) and the `Stage` enum with `StageName`, which
names the point a structured exception was caught at. Header only.

## `hook_context.h/.cpp`

`HookContext` is what used to be the file-scope globals: the real entry points, the config getter,
the shader directory and the loaded `Shaders`, the overlay's outline and colour switches, the
`TemporalSettings` and the `DiagnosticsConfig`, safe mode, the first-warped callback, the hook
attempt counters, the mutex, the `Status`, the evaluate counter, the feature map, the `Graveyard`,
the crash record. One instance, reached through `Ctx()` — a function-local static, so no module
depends on another one's initialization order. The small helpers that only touch the context live
here too: `CallCreate/CallEvaluate/CallRelease` (which go through the forwarder), `SetReason`,
`LoadShaders`, `WaitForGpu`, `DrainGraveyard`, `NotifyFirstWarped`, the readers of the individual
`Debug*` switches and the residual blend weight.

## `feature_state.h/.cpp`

`FeatureState` is everything one host handle owns: the real model behind it, the extent it was
created at, the layout in force, the D3D12 adapter and the warp's textures, the descriptor slots
and the source-set ring, the timing ring, the temporal machine, the background job and the gate of
the last buried one. The .cpp holds `EnsureGpu`, which builds those objects for the host's colour
and output formats, and the retirement rules — `BuryReal`, `BuryAsync`, `BuryGpu`, `RetireGpu` —
which never free anything the GPU may still be reading: it goes into the graveyard behind a fence
gate instead.

## `ngx_params.h/.cpp`

The keys of the parameter block that describe extents: `WriteSizes` over the three width/height
key pairs a host may use, and `ReadSubrect` / `WriteSubrect` over the `DLSSNR.<input>Subrect*`
group, plus `kSubrectNames`. Nothing here knows about the warp.

## `host_depth_state.h/.cpp`

One rule: what state the host's depth guide rests in (`HostDepthState`). It is `NON_PIXEL_SHADER_-
RESOURCE` unless `DebugDepthState` overrides it; a depth-stencil guide is logged once, because its
barriers address the depth plane alone. Every barrier chain that touches the depth asks here.

## `timing.h/.cpp`

`DebugTiming` only: `TimingBegin` / `TimingEnd` wrap the model's evaluate in a timestamp pair,
resolved into the feature's readback ring. A slot is read only when it is a full ring older than
the current evaluate, so nothing in flight is ever mapped.

## `temporal_controller.h/.cpp`

The temporal modes. `EnsureTemporal` builds (or rebuilds) the machine for the host's textures,
`PlanTemporal` decides whether this frame runs the model or is interpolated, `TemporalInputs`
translates the host's description into the machine's `FrameInputs`, `TemporalFrameDone` keeps the
counters, `BackgroundModeUsable` / `EffectiveTemporalMode` fall mode 3 back to the synchronous
interpolation when no host queue is registered, and `TemporalReason` records why a mode is off.
The no-warp path (Mode Off with a temporal mode on) lives here as well: `NativeTemporalEvaluate`
and its guarded body run the model at native size and let the machine fill the frames between.

## `warp_recorder.h/.cpp`

What one warped evaluate records. `EvalContext` is the POD that crosses the structured-exception
frames; `RecordPackStage` packs the host's inputs into the slot's textures, `WarpedBody` runs
Pack → model → Unpack and, when a temporal mode is on, the residual or the reprojection,
`RecordBaseUnpack` produces the temporal base (the packed colour unpacked without the model),
`InterpolateBody` is the frame the model rests on, and `RestoreParams` hands the block back the way
the host left it. `FallbackOutput` / `FallbackFromParams` write the host's own colour when no path
produced the frame. `WarpedGuarded`, `InterpolateGuarded` and `RestoreGuarded` are the SEH wrappers:
`__try` cannot live in a frame that unwinds C++ objects, so each body has its own.

## `warp_compute.h/.cpp`

Pack and the colour Unpack as compute dispatches, for a host that evaluates the model on a compute
list (DLSS5-Reshade-AIO with asynchronous NGX compute), where the adapter's full-screen draws cannot be
recorded. It runs the SDK's own texel functions (`shaders/warp_pack_cs.hlsl`, `warp_unpack_cs.hlsl`)
through its own root signature, descriptor ring and constant ring, into the adapter's packed textures
and the feature's unpack targets. `warp_recorder` picks it when `EvalContext::compute` is set;
`pwngx::StateForList` gives every transition the compute equivalent of its direct-list state.

## `async_scheduler.h/.cpp`

Temporal mode 3, the model on its own queue. `AsyncJob` (in the header, because `FeatureState`
holds one) owns that queue, its fences and events, the private copies of the host's inputs, the
output and base pairs, the allocator/list slots and the pass statistics. The .cpp builds the job
(`EnsureAsync`, including the queue priority and the realtime request), records a kick's input
copies on the host's list, runs `WarpedBody` on the background list for the warped path,
reprojects the last residual on every host frame (`AsyncTemporalEvaluate`), and resolves the
submit tag in `OnCommandListExecuted` so the background queue may start.

## `hook_dispatch.h/.cpp`

The three hooked entry points and the state they drive. `HookCreate` creates the model at the
extent the layout asks for (a second NR consumer in the process is left untouched); `HookRelease`
retires the feature under the right gate; `HookEvaluate` is the frame router — apply the layout,
choose between the background mode, the native temporal path, the warp and the plain pass-through,
and fall back when anything declines. `RecreateReal`, `ApplyLayout`, `DesiredLayout`,
`SameLayoutConfig` and `AdoptFeature` (a handle created before the hooks were installed) are here.

## `ngx_hook_api.cpp`

The public surface: `InstallHooks` (Detours, under its own exception guard), `Poll`, which waits
for `nvngx_dlssnr.dll` at present time, and the `pw_ngx::` setters and getters the add-on drives
the hook with — `Configure`, `SetEnabled`, `SetSafeMode`, `SetDiagnostics`, `SetOutlines`,
`SetMotionAdjust`, `SetOutputColorAdjust`, `SetTemporal`, `SetFirstWarpedCallback`, `GetStatus`,
`RegisterQueue` / `UnregisterQueue` and `OnCommandListExecuted`.

## `ngx_common.h/.cpp`

Moved into the folder unchanged. The machinery shared with the DLSS SR interposer:
parameter-block access, the call forwarder, the D3D12 queue registry with its GPU waits and
graveyard, shader loading, format helpers, barriers and the exception record.

## `ngx_temporal.h/.cpp`, `temporal_resources.h/.cpp`

The GPU side of the temporal modes. `temporal_resources` owns what the machine holds — the root
signature, one pipeline per pass, the descriptor-table ring, the RTV heap, every texture, and the
two helpers that turn a frame's inputs into root constants and into a descriptor table.
`ngx_temporal` records the passes on the host's list and owns the order they run in: the expectation
and the motion accumulation every frame; on a full pass the residual, the phase-in source, the
box-filtered residual, the snapshots and the model's own motion vectors; on a carried frame the
reprojection, the cells and the compose.

The pass functions themselves are in `shaders/temporal.hlsl`, a file shared verbatim with the
experimental OptiScaler build (which runs them as compute). The add-on compiles them through
`shaders/temporal_ps.hlsl`, which sets the feature macros this machine binds registers for
(`PW_T_EXPECT`, `PW_T_RAMP`, `PW_T_CELLS`; `PW_T_HISTORY` is not supported — the table stops at t11)
and adds `PSApply`. Keeping `temporal.hlsl` byte-identical with the fork is deliberate: the two
implementations are compared against each other on the bench.

Recording is split by responsibility: `temporal_accumulation.cpp` owns the expectation, depth copies,
motion chains and synchronous model vectors; `temporal_residual.cpp` owns residual adoption, snapshots
and Apply; `temporal_background.cpp` records the host-side work for asynchronous chains and kicks.
`ngx_temporal.cpp` retains lifecycle, promotion and reprojection. Resource allocation and pipeline
creation live in `temporal_resources_create.cpp`; bindings and constants remain in `temporal_resources.cpp`.
`temporal_phase.h` holds the CPU ramp clock, covered by `tests/test_temporal_phase.cpp`.

### Background origins and order (26.28)

K is the kick frame, T its later adoption frame. All temporal draws stay on the host list, with a
fresh descriptor table per draw. The background list only consumes private model inputs.

* Each host frame advances the main expectation/motion for the displayed residual and, unless it
  mirrors the main chain, the pending expectation/motion for K. Both PSExpect draws read the same
  `depthPrev`; only afterwards is the current host depth copied there. On K+1 this contains exactly
  the depth copied into `depthBg` at K, and the pending expectation uses `params.y = 0`. Reading the
  shared host snapshot avoids transitioning `depthBg` while the background model is reading it.
* Before a kick overwrites the previous kick's guides, PSModelMotion validates the pending chain
  (or its promoted main mirror) against `colorBg/depthBg` and that chain's expectation. Its result
  is copied to `modelAccBg` (RG16F); `accBg` retains the unfiltered K-to-old-pass chain for blending.
  `expectKick` freezes K's main expectation for that same residual lookup. Then the kick's colour
  and depth are copied, and a successful kick resets pending motion and expectation validity.
* At adoption, PSResidual reads K's colour/base, depth and output, `accBg`, `expectKick`, and the old
  `colorF/depthF`. PSResidualOld uses those same coordinates and guides, with the new residual at t1
  and the old residual at t2. Neither pass uses T's displacement to index pixels of K. Snapshots
  are replaced only after alignment; then PromotePending swaps both motion and expectation textures,
  states, indices and RTV identities. Adoption never replaces `depthPrev` with K's older depth.
* The adoption reprojection starts at `1/(n+1)`, then advances every shown host frame, with default
  `n = min(N-1, 3)`. If another adoption interrupts a ramp, PSApply first freezes the outgoing mixture
  in an RGBA16F scratch target using a zero-valued null colour SRV. PSResidualOld aligns that mixture
  before replacing its destination. This reuses existing shader bytecode; neither HLSL file changes.

Added storage: two motion-sized RGBA32F pending expectations, one RGBA32F kick expectation, a native
RGBA16F residual-mixture scratch target and a private motion-sized RG16F model-vector copy. The one
previous-host-depth snapshot is shared. New machine resources retire with the machine; the private
model copy retires with AsyncJob behind its existing fences.

Bench the native and warped/base paths at N=1, 2, 4 and 8, including delayed adoption, adoption and
kick on the same frame, idle gaps after adoption, resets while a pass is in flight and adoption before
a fade finishes. Watch forward-flight error drift, disocclusion trails, adoption flicker, GPU state
errors and timing/memory cost. Keep unrelated settings, especially cells, fixed during this bisect:

| Variant | DebugTemporalNoExpect | DebugTemporalPhaseIn | DebugTemporalNoModelMotion |
| --- | --- | --- | --- |
| 26.28 background baseline / all off | 1 | 0 | 1 |
| Expectation only | 0 | 0 | 1 |
| Phase-in only | 1 | -1 | 1 |
| Model motion only | 1 | 0 | 0 |
| All three | 0 | -1 | 0 |

`-1` selects the cadence-derived ramp length. The all-off path retains raw vectors and disables
expectation/ramp shader effects. With the model-motion key absent, background and synchronous modes
both validate accumulated vectors. An explicit `0` enables validation in both paths; `1` disables it
in both. Expected depth stays on by default in background, but background phase-in defaults to off:
the repeat comparison favoured its mean error without the fade. An explicit `DebugTemporalPhaseIn=-1`
still requests the automatic ramp in both paths; positive values select a length. Synchronous
defaults are unchanged. Measurements and
limitations are recorded in
[`tools/bench/BACKGROUND_26_28.md`](../../tools/bench/BACKGROUND_26_28.md).

### Model passes and spread cycles (26.28, local port)

`model_passes.h/.cpp` owns extra feature creation and the common model evaluate. The host handle
continues to map to `FeatureState::realHandle`; two optional real feature-18 handles have independent
NGX histories. Creation uses the same forwarder and temporarily written work sizes/UI setting as
`RecreateReal`. The creation frame records no model evaluates: it carries the existing temporal
result, or presents the raw host colour during initialization. Failed creation is latched until
settings/extent change, with the actual count and a warning exposed in `Status`. Each sequential
extra pass reads a separate work-size staging copy. Failed extra evaluates preserve the previous
successful output. Handles retire through `BuryReal`; staging and spread resources follow the GPU
and background-job gates in `FeatureState`.

`spread_passes.h/.cpp` owns up to two hidden temporal machines, an immutable native raw snapshot,
a carried input texture and the stage/cadence position. Every frame advances the displayed and
hidden motion chains. Stage k consumes stage k-1 carried onto the current raw frame; its residual
is still measured against that raw frame. Hidden stages have no phase-in or cross-cycle residual
blend. Only the final stage updates the displayed machine. `spread_model.cpp` binds the selected
stage's model vectors, packs/evaluates/unpacks through the existing recorder (or evaluates natively),
and restores the host's parameters. `Machine::RecordRaw` reuses the raw-colour shader branch while
preserving the displayed phase clock. Neither temporal shader source changes.

Spreading uses the host queue for every temporal mode, including a requested background mode; the
tab explains this. With spreading off, the background job runs the entire sequential pass chain.
`ModelPasses=1` bypasses these allocations and schedules. INI settings are carried by the existing
`TemporalSettings`/`config_store` path, with schema and round-trip coverage in `test_addon_ini.cpp`.
Bench results and limitations: [`MODEL_PASSES_26_28.md`](../../tools/bench/MODEL_PASSES_26_28.md).
