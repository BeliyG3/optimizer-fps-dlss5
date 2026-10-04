# The ReShade add-on shell, module by module

The shell lives in `hosts/reshade/`, in namespace `ofps::reshade`.
It loads `optimizer-fps-dlss5-core.dll` and consumes the public ABI through
`core/api/ofps_core.h` (ABI 1), plus its public settings schema. No shell module
includes a private core header. The reusable spatial SDK lives separately in `sdk/`.
Delivery filenames and compatibility export names are unchanged. Current
ReShade settings use `[OptimizerFPS]`; old-only `[PeripheralWarp]` is a one-time
migration source. The remote link uses V4.

`addon/sources.cmake` lists `OFPS_ADDON_SOURCES` relative to `hosts/reshade/`.
Core sources are listed only in `core/sources.cmake`.

## Core connection and NGX translation

| Module | Responsibility |
| --- | --- |
| `shell_host.h/.cpp` | `ShellHost : IOfpsHost`, ABI version check/attachment, logging, event delivery and shell-only status |
| `model_host_ngx.h/.cpp` | `ModelHostNgx : IOfpsModelHost`, model lifetime/calls, readiness, identity codec and diagnostic descriptions |
| `ngx_params.h/.cpp` and supporting files | Read the NGX block into `OfpsFrameInputs`, write/restore `OfpsModelInputs`, extent checks, float-slot probing and diagnostic tails |
| `ngx_forwarder_calls.h/.cpp` | Forwarder entry points and native create/evaluate/release calls |
| `ngx_hook_api.h/.cpp` | Detours installation/polling, hook create/evaluate/release, foreign handles and direct-host bypass |

`ShellHost::OnEvent` connects first-warped to the crash marker, settings changes to
INI persistence, device removal to shell status, and `FEATURE_RELEASED` to model-host
cleanup. It never re-enters the core from an event callback. Model hosts remain alive
until deferred model/GPU work has drained and the release event arrives.
`ShellStatus::lastNgxResult` retains the native code for the game; the core reports
its separate `OfpsEvalResult::modelResult`. Logging preserves the existing NGX result
format, host resource/motion/model fields and first-warped/temporal-ready markers.

## Settings and layout

`addon/addon_context.h` holds `AddonState`, reached through `State()`: module handle,
overlay-only controls, motion/color adjustments, `TemporalConfig`, schema-backed
`OfpsSettingsValues` and OptiScaler takeover state. Runtime/overlay state is separate
from the core's synchronized settings and feature state.

`addon/ini_store.h/.cpp` selects `[OptimizerFPS]` or migrates known present keys
from old-only `[PeripheralWarp]` through ReShade's API. It saves only changed,
explicit persisted values; read-only diagnostics are never written back.
`addon/ini_schema.h` and the public core schema provide defaults, ranges and
conversions. `test_settings_schema` covers selection and migration. The public
core schema contains 46 setting IDs.

`direct_host.h/.cpp` replaces the removed `addon/layout_bridge.h/.cpp`. It probes
`Local\OptimizerFpsDirectHost_<pid>` and latches direct-host ownership after a signal;
missing-event lookup is throttled to 250 ms. ReShade then forwards NGX calls untouched
and displays effective core settings as a read-only "applied by OptiScaler" mirror.
The core exposes settings, ranges and `OfpsLayoutPreview` to the UI.

## Runtime, crash guard and queues

`addon/crash_guard.h/.cpp` owns `optimizer-fps-dlss5.session`, safe mode and retry.
The first-warped event reaches the guard before GPU warp work is recorded. Existing
host exit hooks, the 32-bit host watcher and the game-leaving event retain their
clean-exit behavior; a removed device still leaves the marker. These mechanisms are
shell responsibilities, not model or frame logic.

`addon/queue_events.h/.cpp` collects ReShade queue notifications, registers queues
through `IOfpsCore` and reports executed command lists. The core owns submission
serials, descriptor reuse and deferred retirement. `Housekeeping` runs from present;
queue callbacks do not reach internal GPU structures.

`queue_submit_hook.h/.cpp` hooks `ID3D12CommandQueue::ExecuteCommandLists` for a host whose D3D12
device ReShade never wraps (OptiScaler's own device in D3D11 games): no `execute_command_list`
event ever reports its queue. It swaps the entry in the D3D12 runtime's queue vtable (one atomic
pointer write, no code patching) and pins the add-on module for the session. Menu mode asks for
it (`MenuHostQueue::UnobservedFor`, from a host evaluate, MenuMode on) and `queue_events` installs
it once with two sinks for calls made outside the add-on, the core and ReShade: menu mode's
observer before the native call, the core's and the readable guides' completion tracking after it.
A new graphics/compute queue is registered with the core on the next present (MenuMode on). The
entry goes back at process exit.

`addon/addon_main.cpp` is the entry/composition layer: ReShade registration, overlay
and queue handlers, settings initialization, present work and unload. Existing
`Passive`, `TraceExit`, debug-layer and remote-overlay behavior stays in the shell.

## Overlay, remote tab and exports

`addon/overlay.h/.cpp` and its sections render the current controls, status banner,
layout preview and diagnostics. They read `OfpsStatus`/status rows and apply
`OfpsSettingsValues`; labels, ImGui IDs and persisted controls retain their behavior.

`addon/optiscaler_link.h/.cpp` finds a public OptiScaler in the process (product name in the version
resource), reads its `OptiScaler.ini` (menu key, `SpatialCompression`, `Passes`) and, when it is
there, draws the settings window beside OptiScaler's menu through `reshade_overlay`.

`addon/remote_host.h/.cpp` publishes core and shell snapshots through
`Local\OptimizerFpsRemoteV4` and edits the Feeder optical-flow config.
Remote edits use the same core setters and INI writers as the local tab.
`hosts/remote32/remote_main.cpp` and the other `hosts/remote32/` modules implement the separate 32-bit
`peripheral_warp_reshade_remote` target: no frame core, renderer or Detours.

`addon/exports.cpp` retains `PeripheralWarpSetLayoutV1` and
`PeripheralWarpSetTemporalV1` for bench and compatibility consumers, and adds
`OptimizerFpsSetSettingV1` for one schema setting. `NAME` remains unversioned
because ReShade uses it in its disabled-add-on list.

## Menu mode

All in `hosts/reshade/`, one responsibility per file. User-level description: [RESHADE_ADDON.md](../RESHADE_ADDON.md#menu-mode);
bench results: [menu-mode-acceptance.md](menu-mode-acceptance.md).

| Module | Responsibility |
|---|---|
| `menu_settings.{h,cpp}` | The MenuMode checkbox, Mode and TemporalMode, cached from `OFPS_EVENT_SETTINGS_CHANGED`; a settings epoch that advances when anything but MenuMode changes |
| `menu_state.{h,cpp}` (pure) | Entry/exit rule (150 ms and 5 presents; the next host evaluate ends a run), blockers, suspension, stop, the tab's status line |
| `menu_param_book.{h,cpp}` (pure) | Per-generation trace of the runtime's reads, immutable snapshots with exact result codes, aux refusal, tags, the pass block, the shape check |
| `ngx_param_shim.{h,cpp}` | `TraceParams`, `OwnParams` (absent keys answer the host's code), the exit-reset wrapper |
| `menu_params.{h,cpp}` | Host side: the traced evaluate wrapper, `DebugMenuOwnBlock`, the host shape, the one-generation snapshot at a host evaluate |
| `menu_guides.{h,cpp}` | Depth snapshot sets copied on the host's list (motion is a zero texture), reserved per pass, retired through the core |
| `menu_host_queue.{h,cpp}` | GPU fences between the private queue and a host NR queue other than the present queue |
| `menu_outstanding.{h,cpp}` | Proof, bounded and off the present path, that menu passes finished before the core may free the watched feature's model; a slow pass is held (evaluates withheld without waiting), only a removed device quarantines |
| `menu_fence.h` | Fence completion as menu mode relies on it: a removed device (`UINT64_MAX`, `GetDeviceRemovedReason`) never counts as passed |
| `menu_colour.{h,cpp}` | Colour space and format check (the frame goes as it is), the copy shader's pipeline |
| `menu_pipeline.h` | Public entry points of menu mode (hook, queue events, present, drain, tab) |
| `menu_pipeline_state.h` | Pipeline internals shared by the files below |
| `menu_pipeline.cpp` | The pipeline and its lock, present entry, status, the `destroy_swapchain` drain of the one swap chain menu mode serves (one 500 ms deadline, waited without the lock) |
| `menu_pipeline_host.cpp` | Host-evaluate side: the watched feature, the end of a run, the exit reset, the host queue's exit order |
| `menu_pipeline_step.cpp` | One D3D12 present under the state machine: blockers, entry, end of a run |
| `menu_pipeline_gpu.cpp` | Private queue, fences and rings, size-bound resources, graveyard, drain |
| `menu_submit.cpp` | One menu present on D3D12: capture, pass, write-back, fences |
| `menu_model_pass.cpp` | The direct model pass: convert in, NR evaluate with the own block, convert out |
| `menu_core_pass.{h,cpp}` | The menu frame through the core's feature (Uniform/Peripheral layout, and the sync cadence): the frame's outcome (untouched, written, last output shown again) and the redo as a full frame when a carried frame has no flow field |
| `menu_core_flow.{h,cpp}` | The core's motion source in menus: switched to optical flow at a run's first core frame and back at the run's end, the flow check after each core frame, the latched "optical flow is not available" state, the retry before the game's next evaluate |
| `menu_flow_switch.{h,cpp}` (pure) | The switch state: on, owed (a refused switch-back), and the host-evaluate gate that retries it or withholds the evaluate |
| `menu_gpu_time.cpp` | Private-list GPU timestamps; the slow-pass budget |
| `menu_bridge_d3d11.{h,cpp}` | D3D11 swap chains: the shared-texture bridge to the host's D3D12 device |
| `menu_pipeline_d3d11.cpp`, `menu_pipeline_d3d11_gpu.cpp` | The D3D11 pipeline, and the bridge's lifetime towards the core (queue registration, keying, drain) |
| `menu_marker.{h,cpp}`, `menu_dump.{h,cpp}`, `menu_dump_d3d11.cpp`, `menu_bridge_debug.cpp` | Diagnostics behind `DebugMenuPass=1` / `DebugMenuDump=1` and, for the bridge's debug-layer messages, `DebugMenuBridgeCanary=1`; `DebugMenuNoFlow` 0-3 fakes a missing (1) or failing (2, 3) optical flow for the bench |

For frame protocol, resource states and retirement details, see
[NGX_MODULES.md](NGX_MODULES.md). For public contracts, see [API.md](../API.md).
