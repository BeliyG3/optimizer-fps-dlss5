# The ReShade add-on shell, module by module

Stage 27.D2 split `adapters/reshade/producer.cpp` (~1 630 lines, one translation unit of `g_*`
globals and free functions) into `adapters/reshade/addon/`. Nothing changed but where the code
lives: the same 21 persisted ini keys, the same log strings, the same order of ImGui calls in the
tab. Everything internal lives in one namespace, `pw_addon`, so the modules can see each other
without a global; the NGX interposer is reached only through `adapters/reshade/ngx_hook.h`
(`pw_ngx::`), which this side treats as an opaque API.

The build list is `addon/sources.cmake` (`PW_ADDON_SOURCES`, paths relative to `adapters/reshade`).

## `addon_context.h`

`AddonState` is what used to be the loose settings globals: the add-on's module handle, the two
diagnostic outline switches and the Work-shift switch, the motion-vector diagnostics
(`motionScaleAdjust`, `motionInvert`), the output colour compensation (`brightnessPercent`,
`gamma`), the `pw_ngx::TemporalSettings` and the OptiScaler takeover flag. One instance, reached
through `State()` — a function-local static in an inline function, so no module depends on another
one's initialization order. Everything in it is touched from ReShade's runtime thread only and
carries no lock; the warp configuration itself is the exception and lives in `config_store`.
Header only.

## `config_store.h/.cpp`

The settings: the one validated `pw::ConfigV2` the NGX interposer reads (behind `g_mutex`, because
the getter `ConfigForNgxHook` is polled from the render thread), and all of the ReShade.ini
`[PeripheralWarp]` I/O: `Save*ToReShadeIni`, `LoadConfigFromReShadeIni`, `LoadPersistedConfigOnce`
and `LoadDiagnosticsFromReShadeIni`,
which reads the read-only `Debug*` keys once at load and hands them to `pw_ngx::SetDiagnostics`.
Nothing here ever writes a read-only key back. `StoreConfig` validates and stores; *applying* an
edit is `ApplyConfig` in `layout_bridge`, because an edit may have to go through OptiScaler first.

## `ini_schema.h`

The ini schema without ReShade (stage 27.T): the section name `kIniSection`, `kIniKeys[]` — the
complete, `static_assert`ed list of the 21 keys the add-on writes — `TemporalModeFromIni` with its
`static_assert`s (an ini `TemporalMode=2` falls back to 1), the `AddonPersisted` struct with the
defaults of everything that is persisted, and the `LoadFromStore` / `SaveToStore` pair (plus the
per-group `Save*ToStore`) that map the keys onto it, including the clamps applied on load
(`TemporalEvery` 1..8, `TemporalMaxQueue` 0..8, `Brightness` ±20 %, `Gamma` 0.7..1.4; the layout
itself is not clamped, an invalid one is rejected by `pw::ValidateConfig`). They are templates over a
*Store* — anything with `GetInt/GetFloat/SetInt/SetFloat` — so `config_store.cpp` binds them to
`reshade::get_config_value` / `set_config_value` and `tests/test_addon_ini.cpp` binds them to an
in-memory ini, which is how the add-on's persistence is covered by ctest
(`peripheral_warp_addon_ini`) without ReShade. Header only.

## `crash_guard.h/.cpp`

The 26.16 crash guard. `CrashGuardInit` runs once from `DllMain` with the directory the add-on was
loaded from: it reads `CrashGuard`, builds the `optimizer-fps-dlss5.session` marker path, discards a
marker whose session ran on for more than 20 s past its first warped frame (a killed helper process,
not our crash) and otherwise puts the session into `pw_ngx::SetSafeMode(true)`. The marker itself is
written from two threads — `CrashMarkerOnFirstWarped` on the render thread just before the first
warped evaluate is recorded, and `CrashGuardOnPresent` every 5 s while warping — so a crash inside
that very first evaluate still leaves a marker. `CrashGuardRetry` is the tab's Retry button;
`CrashMarkerClear` is the clean unload.

## `layout_bridge.h/.cpp`

The OptiScaler side. `ProbeLayoutBridge` looks for `PeripheralWarpLayoutBridgeV1` in the loaded
modules for a few hundred presents; `PullLayoutFromBridge` adopts the consumer's layout when its
generation moves, or — under the takeover, which is the default — keeps forcing OptiScaler's own
spatial warp Off (`ForceBridgeWarpOff`), retrying every present when the write fails (26.21).
`ApplyConfig` lives here because it is the one path that has to offer an edited layout to a linked
consumer before it may be stored and persisted; its refusal flag is `BridgeLayoutRejected()`.
`SetOptiScalerTakeover` is the tab's checkbox: it persists the choice and hands the layout over in
whichever direction the new mode needs.

## `remote_host.h/.cpp`

The 64-bit half of the remote overlay (26.11) and the optical-flow config it edits (26.16). The
shared block `Local\PeripheralWarpRemoteV1` carries settings one way and status the other, published
by a generation counter written last; `RemotePublish`, called from present, applies whatever the
game's 32-bit tab changed (through the same setters and ini writers the local tab uses) and then
republishes the hook status and the applied settings. A version-1 remote writes only the first 76
bytes, so the optical-flow fields it never heard of stay zero and are ignored. The optical-flow half
reads and writes `dlss5-feed-host64.cfg` next to the host exe (`pw_ofa_cfg.h`), which the host
re-reads while it runs; the tab reaches it through `OfaLoaded()` / `OfaSettings()` / `OfaSave()`.

## `queue_events.h/.cpp`

The three ReShade device/queue handlers. D3D12 graphics queues are only *noted* while the game
creates its device and are handed to `pw_ngx::RegisterQueue` from the first present
(`FlushPendingQueues`, 26.7.4 — Death Stranding DC died in the creation window);
`execute_command_list` forwards to `pw_ngx::OnCommandListExecuted` so the background temporal pass
knows when its input copies were submitted.

## `overlay.h/.cpp`

The tab. `DrawOverlayEmbedded` draws it inside ReShade's Add-ons tab (the default since 26.13) and
`DrawOverlay` into the separate window `FloatingWindow=1` adds; the only difference is that the
floating one is resized to its content. `DrawOverlayBody` keeps the layout controls (mode, colour
filter, zone size, zone pad and offsets, work shift, outlines) and the pending-edit rule that
applies a value on release rather than on every dragged frame; the rest is one function per section
— `DrawStatusBanner`, `DrawOutputColour`, `DrawTemporal`, `DrawOfa`, `DrawDiagnostics` — plus the
drawing helpers `ZoneAxisRectangles`, `ControlWidthFor`, `ResetIconButton` and `DrawZonePad`. The
split is a pure extraction: ImGui hashes widget IDs and its saved window state from the label
strings and the ID stack, so the sequence of calls, the labels and the `PushID` scopes are exactly
what they were.

## `exports.cpp`

`PeripheralWarpSetLayoutV1` and `PeripheralWarpSetTemporalV1`, the two C exports the bench harness
(and a consumer add-on) drive the add-on with. Resolved by `GetProcAddress`, so they are declared in
no header; both must be called from the thread that presents.

## `addon_main.cpp`

The entry point. The `NAME` / `DESCRIPTION` exports (`NAME` carries no version, because ReShade
keys its DisabledAddons list on it), the overlay-visibility event
`Local\DLSS5_ReShadeOverlay_<pid>` that tells the JFO presenter when ReShade's overlay is open, the
`reshade_present` handler that drives everything above once a frame, the 26.7.4 exit trace
(`TraceExit=1`) and `DllMain` — which registers the add-on, the D3D12 debug layer, the diagnostics,
the crash guard, the events and the two overlays, and honours `Passive=1` by registering nothing at
all.

## Not in this folder

`adapters/reshade/producer_remote.cpp` is the 32-bit remote tab, a separate target
(`peripheral_warp_reshade_remote`) that shares nothing with the add-on but `pw_remote_ipc.h` — no
SDK core, no renderer, no Detours. It stays where it is.
