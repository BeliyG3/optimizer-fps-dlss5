# The shared core and NGX shell, module by module

`core/sources.cmake` feeds both the runtime `optimizer-fps-dlss5-core.dll` and
static `ofps_core` (`OptimizerFps::Core`) used by tests. The Windows x64 core
builds independently of the ReShade shell. `hosts/reshade/` and the narrow
OptiScaler integration consume the same ABI 1 contract. The ReShade payload
places the DLL and 21 DXBC in `optimizer-fps-dlss5/` beside the x64 add-on;
the x86 remote tab stays beside the x86 ReShade DLL and connects to the x64
host through IPC V4.

## Public boundary and ownership

`core/api/ofps_core.h` is the single host entry (ABI 1, release 2026.9.1).
`core_impl.h/.cpp` implements the process core, host registration, capabilities and
feature creation/adoption. `core_feature.cpp` implements the feature wrapper and
evaluate/rebuild/release calls; `core_settings.cpp` applies settings and emits changes;
`core_status.cpp` provides status rows and layout previews. `settings/schema.cpp`,
`values.h/.cpp` and `status.h/.cpp` own schema, validation/defaults and presentation
data. The public settings schema is `api/ofps_settings_schema.h`.

The shell has no private-core includes. `IOfpsHost` receives logs/events and
`IOfpsModelHost` owns all model/codec calls. Callbacks cannot re-enter core methods.
Feature release first retires model/GPU work; `FEATURE_RELEASED` is delivered only
after the model host is no longer referenced by deferred work. Hosts remain alive
until unregistered. Borrowed status strings must be copied before the next call of
the same method. No allocator ownership crosses this boundary.

## Core state and frame recording

`core/context.h/.cpp` owns `CoreContext`, reached through `Ctx()`, and the existing
`Status`, `TemporalSettings`, `DiagnosticsConfig` and callback types. The context retains
the feature map, foreign handles, mutex, settings, counters, shaders and graveyard.
`SetReason`, `NotifyFirstWarped`, `LoadShaders`, GPU waits and diagnostic readers remain here.
`core/log.h/.cpp` owns the existing log callback and formatter.

`core/frame/common.h` retains stage codes, private model resource states and GPU aliases.
`core_feature.cpp` validates frame entry; `dispatch.cpp` guards evaluation and
guarantees the model host's `EndFrame` call. `dispatch_body.cpp`
routes explicit inputs between pass-through, warp and temporal paths.
`lifecycle.h/.cpp` retains `DesiredLayout`, `RecreateReal`,
`SameLayoutConfig`, `ApplyLayout` and `CreateModelGuarded`.

`feature_state.h/.cpp` owns each feature's resources and their retirement rules.
`warp_recorder.h/.cpp` records Pack, model, Unpack, base and interpolation passes,
restores resource states and supplies the existing SEH wrappers and fallback output.
`host_shape`, `host_depth_state`, `motion_smooth`, `debug_readback` and `timing`
retain their separate shape, depth-state, smoothing, readback and timestamp responsibilities.
`model_passes`, `spread_passes` and `spread_model` retain sequential and spread model work.

## Temporal code and shaders

`core/temporal/controller.h/.cpp` chooses temporal modes and handles the native path.
`machine.h/.cpp`, `accumulation.cpp`, `residual.cpp` and `background.cpp` record
the existing GPU passes. `resources.h/.cpp` supplies bindings and constants;
`resources_create.cpp` allocates textures and pipelines. `history.h/.cpp` owns saved
passes. `phase.h` holds the CPU phase clock, covered by `test_temporal_phase.cpp`.

`async_entry.cpp` creates the background job and prepares `AsyncTemporalEvaluate`;
the core's submission path handles `OnCommandListExecuted`.
`async_frame.h` carries the shared `AsyncCtx`
and private-data tag. `async_scheduler.cpp` owns private input copies, `AsyncBody`
and `AsyncGuarded`, with host states restored after every copy.

The six temporal/motion shader files now live in `core/shaders/`, byte-identical
to their previous locations. HLSL identifiers and `pwtemporalcontract` are unchanged.
Both fxc and dxc search `sdk/shaders` and `core/shaders`. The manifest still produces
15 temporal DXBC files, alongside five SDK shaders and one motion-smoothing shader.

## GPU support

`core/gpu/queues.h/.cpp` owns queue registration, GPU waits and `GateSet`.
`graveyard.h/.cpp` owns deferred releases, including `IOfpsModelHost::ReleaseModel`.
`submission.h/.cpp` and its supporting modules track pending command-list serials,
queue fences and background tags. `descriptor_pool.h/.cpp` and `constant_ring.h/.cpp`
gate descriptor/set reuse on completion, with bounded waiting and explicit exhaustion
fallback. Retirement combines pending host submissions and background fences. Without
a registered queue retirement waits 16 evaluates before retrying a gate; resources
remain pending if no queue can confirm completion. The delay alone does not free them.
The bounded submission ring uses its own mutex, not a lock-free queue.
`shaders.h/.cpp` loads shader binaries, `barriers.h/.cpp` owns format and texture
helpers, and `crash_guard_seh.h/.cpp` records structured exceptions.
The core no longer includes or stores the SDK D3D11 adapter.

## Explicit frame inputs and NGX shell

`hosts/reshade/ngx_hook_api.cpp` owns `HookCreate`, `HookEvaluate`, `HookRelease`,
Detours, foreign-handle forwarding, installation guards and polling. Before evaluate it snapshots the model host and
calls `ReadFrameInputs` in `ngx_params.cpp`. The shell owns NGX keys, float probing,
subrect defaults, motion diagnostics and model-key descriptions. `ModelHostNgx`
returns the newline-separated motion/resources/model diagnostics and optional probe tail.
`ngx_forwarder_calls` owns all native model entry points and forwarding calls.

`core/frame/frame_inputs` resolves unknown views and copies explicit resources
into model inputs. `core_feature.cpp` copies the caller frame; `dispatch_body.cpp`
routes codec and creation-frame work, and `dispatch.cpp` completes `EndFrame`.
All external resources carry their own resting state and subresource. In-place
color/output uses a single resting state for overlapping subresources; private
model textures use `kModelInputState` and `kModelOutputState`. Planar depth copies
and barriers touch the selected depth plane, leaving stencil alone.
`HostDepthState` defaults to the supplied state; diagnostic overrides remain explicit.

`host_shape` judges the supplied rectangles, latches an unfit host until layout
changes, and emits `HOST_SHAPE_REJECTED` once through the core's host event path.
Fallback copies read frame resources, and native/spread/background temporal paths
preserve the same states and subresource indices. The parameter bridge and all
core NGX reads are removed. `ShellHost` connects `IOfpsHost` to the crash guard, INI
persistence and shell status; the add-on reads `OfpsStatus` and writes
`OfpsSettingsValues` through the ABI.

The frame grid is the feature's native extent, separate from the model's work grid.
Padded output is accepted when its supplied region fits; an unfit/shifted region is
latched as pass-through rather than repeatedly rebuilding the model. Model creation
never evaluates on the creation list; deferred models wait for `ModelReady`, force a
history reset on first use and complete each frame with `EndFrame`. Codec preparation
precedes Pack, and answer resolution follows Unpack; an identity host skips conversion.
The shell restores the NGX block after model calls and keeps raw NGX return codes
separate from `OfpsEvalResult::modelResult`.

`test_ngx_params` uses real WARP textures to check missing subrects, planar depth,
scales and model diagnostics. `test_host_shape` checks padded and shifted output
regions, rejection latching/events, typed-view preservation and depth overrides.

Add-on modules use `ofps::reshade`; references to the ReShade SDK use `::reshade`.
`tools/check-core-includes.py`, registered as `ofps_core_includes` when the core
target exists, rejects shell, ReShade, Detours, D3D11-adapter and parent-relative
includes under `core/`.

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

`core/frame/model_passes.h/.cpp` owns extra feature creation and the common model evaluate. The host handle
continues to map to `FeatureState::realHandle`; two optional real feature-18 handles have independent
model histories. Creation goes through `IOfpsModelHost`, just like `RecreateReal`;
the NGX shell supplies temporary work sizes/UI settings and restores the block.
The creation frame records no model evaluates: it carries the existing temporal
result, or presents the raw host colour during initialization. Failed creation is latched until
settings/extent change, with the actual count and a warning exposed in `Status`. Each sequential
extra pass reads a separate work-size staging copy. Failed extra evaluates preserve the previous
successful output. Handles retire through `BuryReal`; staging and spread resources follow the GPU
and background-job gates in `FeatureState`.

`core/frame/spread_passes.h/.cpp` owns up to two hidden temporal machines, an immutable native raw snapshot,
a carried input texture and the stage/cadence position. Every frame advances the displayed and
hidden motion chains. Stage k consumes stage k-1 carried onto the current raw frame; its residual
is still measured against that raw frame. Hidden stages have no phase-in or cross-cycle residual
blend. Only the final stage updates the displayed machine. `core/frame/spread_model.cpp` binds the selected
stage's model vectors, packs/evaluates/unpacks through the existing recorder (or evaluates natively),
and restores external resource states. The shell's model host restores the NGX parameter block. `Machine::RecordRaw` reuses the raw-colour shader branch while
preserving the displayed phase clock. Neither temporal shader source changes.

Spreading uses the host queue for every temporal mode, including a requested background mode; the
tab explains this. With spreading off, the background job runs the entire sequential pass chain.
`ModelPasses=1` bypasses these allocations and schedules. INI settings are carried by the existing
`TemporalSettings`/`config_store` path, with schema and round-trip coverage in `test_addon_ini.cpp`.
Bench results and limitations: [`MODEL_PASSES_26_28.md`](../../tools/bench/MODEL_PASSES_26_28.md).
