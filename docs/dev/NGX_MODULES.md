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

## `ngx_common.h/.cpp`, `ngx_temporal.h/.cpp`

Unchanged, moved into the folder. `ngx_common` is the machinery shared with the DLSS SR
interposer: parameter-block access, the call forwarder, the D3D12 queue registry with its GPU
waits and graveyard, shader loading, format helpers, barriers and the exception record.
`ngx_temporal` is the GPU side of the temporal modes: the residual, the depth snapshot, the
accumulated motion and the reprojection passes.
