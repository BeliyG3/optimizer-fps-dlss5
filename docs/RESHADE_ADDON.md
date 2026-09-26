# The ReShade add-on

`optimizer-fps-dlss5.addon64` — displayed in ReShade as **Optimizer FPS for DLSS5** — is an NGX
interposer. It installs Detours hooks on the create, evaluate and release entry points of feature 18
(Neural Rendering) in `nvngx_dlssnr.dll` and changes what the model is asked to do: a smaller,
non-linearly compressed frame, and optionally not on every frame.

It is not a ReShade effect and it has no producer/consumer role: the ReShade producer ABI, the
`PeripheralWarp.fx` sample and every effect event were removed in 26.26. The add-on uses ReShade for
three things only — its overlay, its `ReShade.ini`, and the D3D12 command queues it reports through
`init_command_queue`.

Installation and the installer's own options are in [INSTALL.md](INSTALL.md); user-facing symptoms
are in [TROUBLESHOOTING.md](TROUBLESHOOTING.md).

## Files

The source tree separates `sdk/` (spatial math, D3D adapters and shaders), `core/`
(`frame/`, `temporal/`, `gpu/`, `settings/`, `shaders/`) and `hosts/reshade/`.
The shell dynamically loads `optimizer-fps-dlss5-core.dll` and enters it only through
`core/api/ofps_core.h` (ABI 1). `ShellHost` delivers logging/events, `ModelHostNgx`
implements `IOfpsModelHost`, and `ngx_params` owns NGX parameter translation.
The overlay reads `OfpsStatus` and applies `OfpsSettingsValues` through that ABI.
This extraction keeps deployment names, compatibility exports and diagnostic log
formats. Current ReShade settings use `[OptimizerFPS]`; an old-only
`[PeripheralWarp]` section is migrated once, without deleting the old section.
Static `ofps_core` remains for tests; runtime state belongs to one process-wide DLL.

`CoreLoader` serializes lookup/version checking/loading with
`Local\OptimizerFpsCoreLoad_<pid>`. It checks an already loaded module first, otherwise
loads by absolute host-module directory and pins it with `GET_MODULE_HANDLE_EX_FLAG_PIN`.
Version/ABI conflicts leave the shell inactive with a banner naming the first host
(or `unknown host` if metadata is unavailable); it does not load a second core.
Unload unregisters the host while callbacks are alive, without waiting. Normal
`Detach()` outside callbacks/loader lock also calls `Release`; only the last host
shuts down the core. Hot-unload/pre-unload handshake and shell draining remain plan 8.

| File | Where it goes | What it is |
|---|---|---|
| `optimizer-fps-dlss5.addon64` | beside the 64-bit ReShade DLL (in `host64\` for a 32-bit game) | the add-on itself |
| `optimizer-fps-dlss5-core.dll` | beside the x64 add-on | shared frame core, ABI 1, static CRT |
| `nvngx.dll_optimizerfps.dll` | beside the add-on | forwarder; exports `pw_ngx_call_create` / `_evaluate` / `_release` |
| `optimizer-fps-dlss5\*.dxbc` | subdirectory beside the core DLL | 21 compiled shaders |
| `optimizer-fps-dlss5-remote.addon32` | beside the game's 32-bit ReShade DLL | the tab only, for 32-bit games |

The forwarder exists because the NR snippet checks its caller: it accepts calls only from a module
whose file name contains `nvngx.dll`. It is ours; nothing of NVIDIA's is redistributed.

The shader set is twenty `.dxbc` blobs: one vertex shader (`fullscreen_vs`), the four pixel shaders
`pack_ps`, `unpack_ps`, `preview_ps` and `outline_ps`, and the fifteen temporal passes, which are
compute — `temporal_<Name>_cs.dxbc`, the names listed in `shaders/temporal_passes.def`. Pack and
Unpack are required; of the temporal passes, Residual, Accumulate, Reproject and Expect are required
for the temporal modes and the rest degrade gracefully — a missing one disables just the pass it
belongs to, and a missing required one disables the temporal modes with a reason in the tab. "Beside
the add-on" honors `[ADDON] AddonPath` in `ReShade.ini`. Earlier releases shipped the same passes as
pixel shaders (`temporal_*_ps.dxbc`); the installer deletes those when it updates.

The temporal passes are compiled from `shaders/temporal_cs.hlsl`, a thin wrapper that sets the
feature macros and the thread group; the maths itself lives in `shaders/temporal.hlsl`, which is
shared by every host of the core. Compile a pass without the wrapper and it
reads a different register set than the add-on binds.

## Where the tab lives

The settings are drawn inside ReShade's own **Add-ons** tab (`register_overlay(nullptr)`), not in a
separate overlay window. `[OptimizerFPS] FloatingWindow=1` brings a separate window back. The
32-bit remote tab behaves the same way.

**Beside OptiScaler's menu.** When a public OptiScaler build loads ReShade, OptiScaler's menu key
(`[Menu] ShortcutKey` in `OptiScaler.ini`, Insert by default) also opens a window with the same
settings beside OptiScaler's menu; the key closes it again, as does its close button. OptiScaler's
menu cannot host another add-on, so the window is ReShade's, drawn through the `reshade_overlay`
event with the ReShade overlay closed. ReShade reads the mouse from the window messages before the
game's window procedure does, so the window takes clicks while OptiScaler's menu blocks the game's
input. The window is not drawn while the ReShade overlay is open: the tab is there then.

The exported ReShade `NAME` is **Optimizer FPS for DLSS5** (remote: **Optimizer FPS for DLSS5 (tab
for the 64-bit host)**) and carries no version on purpose — ReShade keys `DisabledAddons` on `NAME`,
so a version inside it would re-enable the add-on for everyone who had turned it off on every
release. The release number is in `DESCRIPTION` and in the tab's first line.

The shipped file names changed in 26.26: `optimizer-fps-dlss5.addon64`,
`nvngx.dll_optimizerfps.dll` and the `optimizer-fps-dlss5\` shader folder replaced
`peripheral-warp.addon64`, `nvngx.dll_peripheralwarp.dll` and `peripheral-warp\`. The old
`[PeripheralWarp]` section remains a migration source, while new writes use `[OptimizerFPS]`.
Compatibility exports `PeripheralWarp*V1` retain their names; the legacy POD/export is not an active ownership bridge. The SDK
uses namespace `ofps::sdk`, package `OptimizerFpsSdk` and `OptimizerFps::Sdk*` CMake targets.

Two version numbers live in `cmake/Version.cmake`. `OFPS_SDK_VERSION` is the library's semantic
version, the one `find_package` sees; it moves only when the API or the ABI does.
`OFPS_RELEASE_VERSION` is the add-on release the changelog is written in: the year and the month it
came out, plus a third component for a second release inside one month (2026.9, then 2026.9.1).
Releases up to 26.29 were numbered after the work stage they came out of — that line ended with the
switch, and the old numbers stay in the changelog and in the code comments that cite them.

## The tab

The tab is laid out so that nothing jumps: settings that do not apply in the current mode are
hidden, but every status line whose text changes with the state stays one line (the full text is in
its tooltip when it is wider than the tab), and a group with nothing to show on this host has no
header.

**Status banner.** Green `Optimizer FPS ACTIVE: model WxH of WxH` when the model sees a warped
frame; frames carried by a temporal mode keep the state of the last model frame. Otherwise orange `Optimizer FPS NOT ACTIVE: <reason>`, with the
reason chosen in this priority: module not loaded → not hooked → waiting for feature 18 → mode Off →
the hook's own pass-through reason. A red crash-guard notice with a **Retry warping now** button
takes precedence over both. When a direct host (below) owns the warp, the banner says so instead.
With a public OptiScaler in the process, a red line under the banner warns when OptiScaler's own
peripheral compression (`[DlssNr] SpatialCompression`) is on, and an orange one when it runs extra
model passes (`Passes` > 1); the add-on reads `OptiScaler.ini` every 2 s, so a change made in
OptiScaler's menu shows once OptiScaler has saved it.

**Mode** — `Off`, `Uniform`, `Peripheral`. **Color filter** — `Bilinear`, or
`Auto (soft: wide pre-filter, cubic unpack)`. Auto adapts to the local footprint: Pack applies a
wide tent-like pre-filter (center plus four bilinear taps at ±0.375 of the footprint), Unpack blends
in a soft cubic B-spline reconstruction (four bilinear fetches); both fade in above a footprint of
1.0. The 1:1 zone stays exact bilinear, so only the compressed periphery is filtered.

**Compression** is one collapsible header over the four groups below.

**Zone size** — `Center X/Y (%)`, `Work X/Y (%)`, `Global scale (%)`. Ctrl+click a slider to type a
value; every slider has a reset icon opposite it. The maths, the validity rules and the preset table
are in [CONFIG.md](CONFIG.md).

**Zone position** (Peripheral only) — a miniature of the frame with the raw Work region in orange
and the 1:1 zone in cyan; drag inside it, or use the Offset X/Y sliders and the "Center both"
button. The Work size does not change when the zone moves: the periphery budget is split evenly, but
a side never gets more work pixels than it has native pixels, so the narrower periphery stays at 1:1
and the wider one is compressed harder. The limit is `±((100 − Center) / 2 − 0.5)`, i.e. ±9.5 % at
Center 80. **Shift the compressed region (outer contour)** adds Work shift X/Y: the orange contour
slides while the zone stays, so the side it moves towards is compressed less and the opposite side
more; unticking the box forces both back to 0.

**Outlines** — `Show uncompressed center (cyan)` and `Show Work boundary (orange)`, drawn by the
final native-resolution pass. Peripheral mode only. They do not enter temporal history and do not
change layout generation.

**Output colour (warped frame only)** — `Brightness (%)` (−20…20) and `Gamma` (0.7…1.4). Unpack
writes `(1 + brightness) * rgb ^ (1 / gamma)` in the buffer's linear space. See
[Tone on a warped frame](#tone-on-a-warped-frame).

**Temporal (Neural Rendering cadence)** — see below.

**Motion vectors (optical flow in the 64-bit host)** — present only inside
`dlss5-feed-host64.exe`, recognised by its `dlss5-feed-host64.cfg` sitting next to it. `Source`
(`auto` / `ofa` / `shader`), `Grid` (1 / 2 / 4), `Quality` (`fast` / `medium` / `slow`). The add-on
writes the cfg and the host re-reads it within a second, so the source can be switched while the
game runs.

**Advanced (diagnostics)** — a checkbox at the end of the tab, saved as `ShowAdvanced` in
`[OptimizerFPS]` (off by default). With it off, the Diagnostics group, the core's status lines,
the plain-colour frame counter and the model GPU time are not drawn.

**Diagnostics** (collapsed) — the NGX hook's state and reason, the layout numbers (native, raw Work,
global scale, NR input with its pixel percentage), direct-host ownership,
the temporal counters, and two trees of session-only switches: the motion scale/inversion handed to
the warped model, and the temporal machine's tolerances, smoothing radii, debug view and log. None
of the Diagnostics switches are persisted.

## `[OptimizerFPS]` in `ReShade.ini`

The new section wins if both sections exist, including when a key is absent
from the new section. An old-only `[PeripheralWarp]` section migrates known
present keys once through ReShade's configuration API. Unknown keys and other
sections remain untouched. Diagnostic keys can be read but normal setting saves
do not write them. The installer seeds `[OptimizerFPS]` for a new installation.
Update migrates an old-only section and records installer-owned keys in a
Schema 3 receipt.

### Saved by the tab

The tab writes an edited persisted key explicitly, including an explicit default,
and restores present keys before the first evaluate. Absent defaults are not
materialized by ordinary saves.

| Key | Type | Meaning |
|---|---|---|
| `Mode` | 0/1/2 | Off / Uniform / Peripheral |
| `ColorFilter` | 0/1 | Bilinear / Auto |
| `CenterX`, `CenterY` | float % | width of the 1:1 band per axis (1…99) |
| `WorkX`, `WorkY` | float % | raw work extent per axis |
| `GlobalScale` | float % | uniform scale fused into the same Pack mapping |
| `Flags` | bitmask | `1` extend motion endpoints past the frame edge, `2` input confidence valid |
| `OffsetX`, `OffsetY` | signed float % | zone offset from the frame center (Peripheral) |
| `WorkShiftX`, `WorkShiftY` | signed float % | shift of the raw Work rectangle |
| `WorkShiftEnabled` | 0/1 | shows the work-shift sliders; off forces both shifts to 0 |
| `ShowCenterOutline`, `ShowWorkOutline` | 0/1 | the two diagnostic outlines |
| `Brightness` | float % | −20…20, warped frames only |
| `Gamma` | float | 0.7…1.4, warped frames only |
| `TemporalMode` | 0/1/3 | every frame / interpolate (sync) / interpolate (background). `2` was withdrawn and falls back to `1` |
| `TemporalEvery` | int | frames per model pass: 2…8 sync, 1…8 background |
| `TemporalMaxQueue` | int | 0…8, background mode: GPU frames allowed unfinished when a frame is recorded |
| `ShowAdvanced` | 0/1 | the tab's **Advanced (diagnostics)** checkbox; written only when it is clicked |

`OptiScalerTakeover` and `ForceBridgeWarpOff` are obsolete compatibility inputs;
they are ignored with a log message and are never written as current settings.

The current installer seeds `Mode=2, CenterX=80, CenterY=80, WorkX=90, WorkY=90`
in the legacy section, only where keys are absent. The add-on migrates those
known values when it first loads an old-only INI.

### Read-only keys

Never written back by the add-on: a saved layout can therefore never resurrect one. All of them are
read once, at add-on load, so a change takes effect on the next game start.

| Key | Effect |
|---|---|
| `CrashGuard` | `0` disables the crash guard entirely |
| `FloatingWindow` | `1` draws the tab in its own overlay window instead of ReShade's Add-ons tab |
| `Passive` | `1` loads and registers the add-on and stops there: no hooks, no events, no tab — an innocence test |
| `TraceExit` | `1` logs who calls `ExitProcess` / `TerminateProcess`, with module + offset per stack frame |
| `DebugLayer` | `1` turns the D3D12 debug layer on before the game's device is created and dumps its messages after the hook's passes (slow) |

The `Debug*` diagnostics of the interposer (`hosts/reshade/addon/config_store.h`), all inert by default:

| Key | Effect |
|---|---|
| `DebugTiming` | timestamp queries around the model's evaluate; the GPU time appears in the tab |
| `DebugTemporalReadback` | center texel values printed every 60th interpolated frame |
| `DebugTemporalKeepOutput` | interpolated frames do not write the host's Output |
| `DebugTemporalBlend` | cross-pass residual weight, 0…0.9 (default: the built-in adaptive one) |
| `DebugTemporalDepth` | depth tolerance override, 0…1 (0 disables the test) |
| `DebugTemporalSmooth` | compose radius override in px, 0…128 (0 = off) |
| `DebugAsyncCompute` | background pass on a compute queue instead of a direct one |
| `DebugAsyncNormalPriority` | normal instead of high queue priority |
| `DebugAsyncNoRealtime` | skip the `GLOBAL_REALTIME` request |
| `DebugAsyncLog` | verbose background-pass log (kicks, submissions, adoptions) |
| `DebugAsyncShowPass` | show the last pass's output as is, with no reprojection |
| `DebugHookDelayMs` | delay hook installation so a host creates its model first (exercises the adoption path) |
| `DebugKeepBackbuffer` | hand the native UI/back buffer to the warped model |
| `DebugDepthState` | host depth resting state; `99` (the default) follows the automatic rule |

The add-on reads no environment variables at all; every diagnostic switch is one of these keys.

## How a frame is processed

**Create.** The feature is created at the effective work extent rather than the native one, so the
model processes a smaller frame. A model the host created before the hooks were installed — hosts
commonly pre-create at device init, while Detours may fail to attach for a while during a level load — is
adopted at its first evaluate: the host's handle becomes the key and the model behind it is
re-created at the work extent when the layout asks for it (`ReShade.log`: `feature 18 adopted`).

**Evaluate.** Pack (the SDK's D3D12 adapter, four render targets: color, depth, motion, optional
confidence) → the model at the work extent → Unpack → copy into the host's Output. The NGX parameter
block is restored afterwards, and `DLSSNR.Reset` is handed back to the host, so the host sees exactly
what it passed in.

Motion vectors: the interposer reads the host's `DLSSNR.MVecScale` (the parameter block's float
getter/setter slots are found by a round-trip probe; on `nvngx_dlssnr.dll` 310.8 (the NR runtime) they are 14 and 6, with a double
getter at 13) and converts the host's vectors to native pixels before Pack. NGX vectors point from
the current frame to the previous one. The Diagnostics tree shows what was read — texture, subrect,
scale — and offers a session-only multiplier and sign flip on top; ghosting that appears only with
the warp on means the vectors reaching the model are wrong, and that line in `ReShade.log` is where
to look first.

**Release and layout changes.** The work extent changing re-creates the model; any other layout
field rebuilds the packed slots. The host's D3D12 queue may run five or six frames behind the CPU
(the DLSS 5 D3D11 bridge keeps up to six in flight), so nothing is released while the GPU can still
read it: the add-on registers the D3D12 graphics and compute queues ReShade reports (a consumer
such as DLSS5-Reshade-AIO runs the model on a compute queue), and a resource is buried
only once a gate — an AND over all queues of the device — has passed it. Releases do not block the
CPU. When no queue is known, objects are kept for 16 further evaluates and released afterwards
(`ReShade.log` says so).

**Failure.** Every failed path writes the host's color into its output rather than leaving the frame
unwritten, and reports a counter and a reason in the tab. A structured exception inside a stage
disables the warp (or the temporal mode) for that feature with the stage named in the reason.

**Descriptor sets.** BG3 through the RenoDX D3D11 bridge hands the model different textures every
frame — three color/output pairs and four motion textures in rotation, twelve combinations — with
the queue five to six frames behind. Caching descriptors by input set therefore rewrote descriptors
that queued frames were still reading. The SDK adapter separates external source sets from texture
slots (`sourceSets` in [API.md](API.md)): the hook takes a fresh set from a ring of 32 per evaluate,
keeps the texture slots round-robin, writes the unpack sets once per slot, and the temporal machine
allocates a fresh descriptor table per draw from a ring of 128.

## Temporal modes

The "Temporal" group has three settings; everything else in the machine is fixed at values verified
on the bench and in game.

* **Temporal mode** (`TemporalMode`) — `Every frame`; `Interpolate: full NR every N-th frame (sync)`;
  `Interpolate: model in the background (async)`.
* **N** (`TemporalEvery`) — 2…8 sync, 1…8 background.
* **GPU frames queued ahead** (`TemporalMaxQueue`, background only, default 2).

Fixed: depth tolerance 0.05, color tolerance 0.08 (chromaticity first, luma at twice the
tolerance), no motion limit, hole fill on, Catmull-Rom resampling on, warp base on, residual age
limit 8 frames in background mode, guided smoothing radius 24 px, motion-vector search radius 16 px.

When Mode is `Off` the interposer still drives the cadence, on the native model.

### Interpolate (sync)

A full pass runs every N-th frame and records the residual `R = output − color`. Frames in between
show the current color plus `R` re-projected along the host's motion vectors, accumulated over the
skipped frames. The residual is dropped where the current depth no longer matches the residual
frame's depth at the re-projected position, where the color no longer matches (which catches wrong
vectors, disocclusions the depth test misses, HUD and transparency), and at every rejected link of
the displacement chain — the chain is validated step by step, not only at its endpoint, so a pixel an
occluder crossed does not inherit the occluder's history.

Rejected pixels are not left without the model's contribution: they take a box-filtered residual
(native/16 per axis, refreshed on every full pass), searched around the reprojected position in two
rings of eight points at 24 and 48 px for a depth match, scaled by the luma ratio to the current
frame and applied along the pixel's own chroma. Without that fill they show the raw color, which in
a tone-mapping host is 5–8 % darker than its surroundings — a dark rim along every moving silhouette.

Three refinements matter for how it looks: the acceptance weight is averaged over nine taps 3 px
apart, so the boundary between reprojected residual and fill is a ramp that does not jitter frame to
frame; the depth test falls off smoothly from the tolerance to twice it rather than cutting, because
thin geometry gives noisy depth; and the compose pass smooths the applied addition over 16 taps on a
noise-rotated disc where acceptance is below 0.5, weighting each tap by the current frame's own
color, depth and acceptance.

On every full pass the new residual is blended with the previous one moved to this pass's frame,
Catmull-Rom resampled, clamped to the 3×3 neighbourhood of the new residual and only where the depth
still matches. The weight is adaptive: 0.6 where the pixel moved less than 2 px and its color is
unchanged, fading out by 6 px of motion or half the color tolerance. This is what stops the teeth,
nostrils and lips of a talking face snapping between pass-to-pass versions of the model's output.

Frame times alternate long/short. Fine detail on moving objects refreshes at the full-pass rate.

### Interpolate (background)

The model runs on its own D3D12 queue on private copies of the host's color, depth and motion — and,
in the warped path, Pack → model → Unpack all on that queue. One pass is in flight at a time; N sets
how often a new one starts (1 = continuous). Every displayed frame is the current color plus the
last finished pass's residual moved along the accumulated vectors, with the hole fill; the plain
color until the first pass completes.

Expected depth travels with both chains and is promoted with the finished pass. Expected depth and
model-motion validation are enabled by default. Background phase-in is off: repeated measurements
with free GPU memory favoured the mean error of the two-feature combination. An explicit
`DebugTemporalPhaseIn=-1` enables the automatic adoption fade over `min(N-1, 3)` carried frames;
a positive value selects its length. Synchronous phase-in remains automatic by default.
`DebugTemporalNoModelMotion=1` disables validation in both modes; an absent key enables it.
See [the background bench report](../tools/bench/BACKGROUND_26_28.md).

Two accumulation chains run: one to the residual on screen, one to the frame of the pass in flight.
The second becomes the first when a pass is adopted, which is what gives each new pass the
displacement to the *previous pass's* frame rather than a one-frame vector — without it the model's
history is misaligned by age−1 frames on every pass.

Synchronization is queue-side only: the kick's host list is tagged with private data, ReShade's
`execute_command_list` event records the queue, the signal is issued at the next evaluate so it is
queued behind the list, and the background queue waits on it. The host queue waits for the pass only
when the residual reaches the age limit. On a layout change, a re-created model or a released
feature, the pass in flight is waited for on the CPU and whatever it still uses goes to the
fence-gated graveyard.

The queue asks for `GLOBAL_REALTIME` priority (`SeIncreaseBasePriorityPrivilege` is requested first,
`HIGH` is the fallback) and logs the outcome as `background queue priority: …`.

**It needs a registered host queue.** Where the consumer creates its D3D12 device before ReShade is
in place — always the case when OptiScaler loads ReShade — no queue is ever registered, the synchronous
interpolation runs instead, and both the tab and the log say `background mode unavailable here (no
registered host queue)`.

Where the residual's age comes from: the kick's copies sit in the host's list, which the GPU reaches
only after the frames already queued ahead of it (5–6 in BG3), then the pass itself, then up to one
frame until the next evaluate notices. Since the next kick starts at the adoption, the residual on
screen ages from `a` to `2a−1`. `GPU frames queued ahead` attacks the first part: a fence per
evaluate on the host's queue, and the CPU waits before recording a frame until at most that many are
unfinished — the GPU stays busy, the queue stops piling up, input latency drops. An in-game frame-rate
cap does the same job. On activation the CPU also waits once for everything the host has queued.

Frame times are more even, which is the point: the synchronous mode's long/short alternation reads as
judder even when the average is higher.

### The warped base

With the warp on, the model's output is the packed frame unpacked, so a residual measured against
the host's raw color would also contain `unpack(pack(x)) − x` — the compression blur of the
periphery, a function of screen position rather than of the scene. Re-projected along the motion
vectors it landed on the sharp color of another position, which showed as periphery shimmer at the
full-pass rate and trails along moving edges, only with the warp on. Every temporal frame is now
based on the frame's own color through Pack → Unpack without the model (an extra Pack + Unpack on
interpolated frames, ~0.5 ms), so the residual holds only the model's contribution.

### Measured

RTX 4080 SUPER, RenoDX NR on a 3456×1944 packed frame: a full pass costs 13–15 ms; Interpolate lifts
53 fps (18.8 ms) to 86 (11.6 ms) at N=2, 109 (9.2 ms) at N=3 and 127 fps (7.9 ms) at N=4. Warped, at
a 60 fps budget, an interpolated frame against every-frame scores periphery edge correlation 0.976
(sync) and 0.974 (background), the center unchanged at 0.98.

The model's own tone statistics lag when frames are skipped: a full pass after N−1 skipped frames
comes out darker than every-frame while the scene changes (about 0.3–0.6 % at N=2, up to 2.7 % at
N=4 right after a scene change), regardless of which vectors it is given. With the consumer's
Intensity / LocalTone / LocalStructure at 0 the difference disappears. N=2 and Interpolate are the
recommended starting point.

## Tone on a warped frame

On the offline bench the model darkens a *native* frame by about 0.8 % at Intensity 2 / Local Tone 2,
and does so less on a warped frame (Peripheral 80/90 about +0.65 %, Uniform 90 about +1.6 % relative
to native NR). At Intensity 0 the difference between native and warped is within 0.2 %. Pack and
Unpack are linear averages and add no gain, and the host hands the model no UI/back buffer either
way — so the shift comes from the model's tone processing depending on the frame scale, not from the
warp.

The Brightness and Gamma sliders compensate. Because Unpack applies them before the host decodes the
model's output, the effect on the final picture is not 1:1 with the number (on the bench −0.64 %
brightness moved the mean by about −1.5 %). Tune by eye.

## OptiScaler

Public OptiScaler builds with Neural Rendering (Dagherbou OptiScaler_DLSSNR, wilsjo2
OptiScaler-DLSSNR-PreSR-Multipass) run NR themselves and, with `[Plugins] LoadReshade=true`, load
ReShade and this add-on. OptiScaler creates the NR model before ReShade is loaded, so the hook
**adopts** it (`feature 18 adopted (created before the hooks were installed)`), re-creates it at the
work size and warps from then on; the rest works as with any other consumer. Checked on bench12
(native D3D12) and on the D3D11 stand through OptiScaler's D3D11-on-D3D12 bridge, including
wilsjo2's NR before the upscaler and OptiScaler's own frame generation — see
[the plan 8 report](dev/plan8-public-optiscaler.md). Two consequences: background mode runs
synchronously (no host queue is visible), and a build with its own peripheral compression must have
it off, or the frame is compressed twice.

**Direct-host protocol.** A program that links the core itself can take NR over from the add-on: it
declares ownership with the manual-reset event `Local\OptimizerFpsDirectHost_<pid>` and calls
`SetEvent` before its first NR create. The add-on activates direct-host mode only after observing
the signaled state; auto-reset events are forbidden because the probe would consume the signal.
Once detected, ownership stays latched for the lifetime of the add-on's probe. Every NGX hook checks
this event before looking up the core or checking its ABI. Access/wait errors fail closed; a late
signal forwards original handles without adoption and logs one warning. The ReShade NGX hooks then
forward calls untouched. The direct host owns core settings; the add-on does not apply or save its
local core settings and shows the effective settings as a read-only mirror. Shell-only ini settings
and diagnostics continue to load normally. No shipped product uses this protocol in 2026.9.1; public
OptiScaler builds do not signal the event, and the add-on warps their NR as described above.

`OptiScalerTakeover` and `ForceBridgeWarpOff` in `[PeripheralWarp]` or `[OptimizerFPS]`
are obsolete and ignored with a log message. The layout bridge and takeover checkbox
have been removed. Existing obsolete INI entries are neither rewritten nor deleted.

## A second NR consumer in the process

A second consumer's create call goes straight to the original entry points — no forwarder of ours, no
state of ours — and its feature is adopted only once the warped host's feature is gone. A feature
that the one-warped-feature rule made pass-through takes the layout as soon as it is the only one
left, which matters because RenoDX re-creates its feature before releasing the old one.

In Control, where `renodx-control-rr` and `renodx-dlss5` both ask for NR: no crash,
`renodx-dlss5` gets `0xBAD00002` (the model's own caller check) and the NR of `renodx-control-rr`
keeps working, warped.

## Crash guard

`optimizer-fps-dlss5.session` is written next to the add-on from the hook, right before the first warped
evaluate, refreshed every 5 s while the session runs, and removed on a clean unload. Writing it from
the hook rather than at the next present matters: a crash guard that waited for a present never
recorded sessions that died inside the render thread.

If the marker is found at start-up, NR calls are forwarded untouched for this run and the tab shows
a red notice with **Retry warping now**. A session that lived more than 20 s after its first warped
frame is not counted as a crash — Feeder's `host64` is terminated at game exit and never unloads
cleanly, and so are games the player kills. `CrashGuard=0` disables the guard. In
DLSS5-Reshade-AIO's `AIO DLSS5 32-bit Wrapper.exe` the guard is off unless `CrashGuard=1` is set:
AIO ends that process from outside on every "apply settings", which no marker can tell from a crash.

**Status line.** While a model is hooked, `ReShade.log` gets one line every 30 s: ACTIVE, NOT ACTIVE
or SAFE MODE, how many frames went through the add-on or were shown as plain colour in that time,
the model and native sizes, the temporal mode, and the reason the last one did not warp. It is there
for helper processes whose tab is out of reach.

## Remote tab for 32-bit games

In the DLSS 5 x86 kits a 32-bit game runs ReShade x86 with `dlss5-feed.addon32`, and Neural
Rendering runs in `host64\dlss5-feed-host64.exe`, where ReShade x64 loads RenoDX and
`optimizer-fps-dlss5.addon64`. Our tab would therefore be drawn into a window the player never sees.
`optimizer-fps-dlss5-remote.addon32`, placed next to `dlss5-feed.addon32`, draws the same tab in the game
and talks to the host through shared memory.

The protocol is [`hosts/remote32/ipc.h`](../hosts/remote32/ipc.h): a fixed
POD V4 block in `Local\OptimizerFpsRemoteV4`. Each writer brackets its copy
with odd/even sequence values, then publishes its generation. The host snapshot
carries the core's applied values, ranges, availability reasons, status rows and
shell/Feeder state. The remote tab sends settings and Feeder OFA edits through
the same block. The host validates magic, version and size before using it.
Retry uses `Local\OptimizerFpsRemoteRetryV4`; both sides observe the process
leaving event. A V4 tab detects the old `Local\PeripheralWarpRemoteV3` block
only to display a version mismatch and never writes into it.

The host applies remote edits through the same core and INI writers as the local
tab. The generation seen at startup is adopted without applying, so a block
left over from an earlier session cannot override this process's settings.

The 32-bit side ([`hosts/remote32/remote_main.cpp`](../hosts/remote32/remote_main.cpp)) has no SDK core,
no D3D and no Detours — only the ReShade API and ImGui headers. Its banner is green, orange, or grey
(`the 64-bit host ... is not running yet`) when the mapping is missing or the heartbeat is older
than three seconds. The controls mirror the x64 tab and are seeded from `applied` when the host is
first seen. A note in the tab points out that compression in these kits may already be done by the
x86 feeder before the host sees the frame: if the Feeder build already compresses the frame, `Mode`
should be `Off` there, to avoid double compression. The temporal modes still run with `Mode` `Off`.

Build: `cmake --preset windows-x86-remote` then `cmake --build --preset windows-x86-remote-release`;
the output is `out/build/x86-remote/hosts/remote32/Release/optimizer-fps-dlss5-remote.addon32`. The
x64 add-on is unchanged for every other game: without a remote overlay the block is written and never
read.

## Source map

| | |
|---|---|
| `hosts/reshade/addon/` | the add-on shell: registration and `DllMain` (`addon_main.cpp`), the overlay, `[OptimizerFPS]` persistence and legacy migration, the direct-host gate, the crash guard, the queue registration, the remote host side |
| `hosts/reshade/ngx_hook_api.cpp`, `ngx_params.cpp` | the NGX interposer: Detours, feature interception and translation to the core ABI |
| `core/` | Pack/model/Unpack, deferred resources, temporal processing and background scheduling shared by hosts |
| `hosts/reshade/ngx_forwarder/` | `nvngx.dll_optimizerfps.dll` |
| `hosts/remote32/remote_main.cpp` | the 32-bit remote tab: what it draws |
| `hosts/remote32/remote_link.{h,cpp}` | the tab's half of the shared block: finding it, reading any known version, sending an edit |
| `hosts/reshade/addon/config_store.h` | the `Debug*` switch block |
| `hosts/remote32/ipc.h`, `hosts/reshade/feeder_ofa_cfg.h` | the shared-memory protocol; the host's optical-flow cfg |
| `core/shaders/temporal.hlsl` | the temporal maths compiled into the core's shader payload |
| `core/shaders/temporal_cs.hlsl` | compute entry points for the temporal passes |

The per-file maps of those two folders are [dev/NGX_MODULES.md](dev/NGX_MODULES.md) and
[dev/ADDON_MODULES.md](dev/ADDON_MODULES.md).

## Deployment

The build deployed to games is the static-CRT one, `out/build/x64-mt` — see [BUILD.md](BUILD.md).

### Model passes

The Neural Rendering tab includes **Model passes** (1?3, default 1) and **Spread passes over frames**
(default on). Their `[OptimizerFPS]` keys are `ModelPasses` and `SpreadPasses`. Each additional pass
runs an independent model on the previous pass's output. With temporal mode on, spreading runs one
stage per host frame, carries the previous completed result until the new cycle finishes, and raises
N to at least the pass count. Spreading uses the host queue even if background mode is selected;
with spreading off, background mode retains its background queue.

The tab shows the running/requested count and creation/allocation/evaluate warnings. Extra passes
are expensive: an earlier OptiScaler build measured 13–15 ms per extra evaluate at 4K on a 4080 SUPER, roughly 1 GB for
a second model and another ~0.5 GB for spread carry buffers. Gains fade after pass 2, and each pass
can darken the image by about 1%. A 007 First Light test with that build exceeded VRAM capacity at two passes
and reached 0.4–0.6 seconds per frame. These observations are also in the tooltips.

See [local bench report](../tools/bench/MODEL_PASSES_26_28.md); this port has not been installed into or
tested in games.
