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
core schema contains 45 setting IDs.

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

For frame protocol, resource states and retirement details, see
[NGX_MODULES.md](NGX_MODULES.md). For public contracts, see [API.md](../API.md).
