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

| File | Where it goes | What it is |
|---|---|---|
| `optimizer-fps-dlss5.addon64` | beside the 64-bit ReShade DLL (in `host64\` for a 32-bit game) | the add-on itself |
| `nvngx.dll_optimizerfps.dll` | beside the add-on | forwarder; exports `pw_ngx_call_create` / `_evaluate` / `_release` |
| `optimizer-fps-dlss5\*.dxbc` | beside the add-on | compiled shaders |
| `optimizer-fps-dlss5-remote.addon32` | beside the game's 32-bit ReShade DLL | the tab only, for 32-bit games |

The forwarder exists because the NR snippet checks its caller: it accepts calls only from a module
whose file name contains `nvngx.dll`. It is ours; nothing of NVIDIA's is redistributed.

The shader set is ten `.dxbc` blobs: one vertex shader, `fullscreen_vs`, and nine pixel shaders —
`pack_ps`, `unpack_ps`, `preview_ps`, `outline_ps` and the five temporal passes
`temporal_residual_ps`, `temporal_downsample_ps`, `temporal_accumulate_ps`, `temporal_reproject_ps`,
`temporal_compose_ps` (see `cmake/CompileShaders.cmake`). Pack and Unpack are required; the rest
degrade gracefully (a missing `temporal_*.dxbc` disables the temporal modes with a reason in the
tab). "Beside the add-on" honors `[ADDON] AddonPath` in `ReShade.ini`.

## Where the tab lives

The settings are drawn inside ReShade's own **Add-ons** tab (`register_overlay(nullptr)`), not in a
separate overlay window. `[PeripheralWarp] FloatingWindow=1` brings a separate window back. The
32-bit remote tab behaves the same way.

The exported ReShade `NAME` is **Optimizer FPS for DLSS5** (remote: **Optimizer FPS for DLSS5 (tab
for the 64-bit host)**) and carries no version on purpose — ReShade keys `DisabledAddons` on `NAME`,
so a version inside it would re-enable the add-on for everyone who had turned it off on every
release. The release number is in `DESCRIPTION` and in the tab's first line.

The shipped file names changed in 26.26: `optimizer-fps-dlss5.addon64`,
`nvngx.dll_optimizerfps.dll` and the `optimizer-fps-dlss5\` shader folder replaced
`peripheral-warp.addon64`, `nvngx.dll_peripheralwarp.dll` and `peripheral-warp\`. The ini section
Internal identifiers (the `[PeripheralWarp]` ini section, exports `PeripheralWarp*V1`, C++
namespaces, CMake targets) keep the historical name PeripheralWarp so existing installs and the
OptiScaler bridge keep working.

Two version numbers live in `cmake/Version.cmake`: `PW_SDK_VERSION` (the library and the
`find_package` version) and `PW_RELEASE_VERSION` (the 26.x add-on release the changelog is written
in).

## The tab

**Status banner.** Green `Optimizer FPS ACTIVE: model WxH of WxH, N frames` when the frame the
model sees is warped right now. Otherwise orange `Optimizer FPS NOT ACTIVE: <reason>`, with the
reason chosen in this priority: module not loaded → not hooked → waiting for feature 18 → mode Off →
the hook's own pass-through reason. A red crash-guard notice with a **Retry warping now** button
takes precedence over both. When OptiScaler applies the warp itself the banner says so instead.

**Mode** — `Off`, `Uniform`, `Peripheral`. **Color filter** — `Bilinear`, or
`Auto (soft: wide pre-filter, cubic unpack)`. Auto adapts to the local footprint: Pack applies a
wide tent-like pre-filter (center plus four bilinear taps at ±0.375 of the footprint), Unpack blends
in a soft cubic B-spline reconstruction (four bilinear fetches); both fade in above a footprint of
1.0. The 1:1 zone stays exact bilinear, so only the compressed periphery is filtered.

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

**Diagnostics** (collapsed) — the NGX hook's state and reason, the layout numbers (native, raw Work,
global scale, NR input with its pixel percentage), the OptiScaler link and its takeover checkbox,
the temporal counters, and two trees of session-only switches: the motion scale/inversion handed to
the warped model, and the temporal machine's tolerances, smoothing radii, debug view and log. None
of the Diagnostics switches are persisted.

## `[PeripheralWarp]` in `ReShade.ini`

### Saved by the tab

Twenty-one keys, written on every accepted edit and restored at load, before the first evaluate.

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
| `OptiScalerTakeover` | 0/1 | warp inside OptiScaler's NR call (default on when a bridge is linked) |

The installer seeds `Mode=2, CenterX=80, CenterY=80, WorkX=90, WorkY=90` and only for keys that are
not already present.

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

The `Debug*` diagnostics of the interposer (`adapters/reshade/diagnostics.h`), all inert by default:

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
read it: the add-on registers the D3D12 graphics queues ReShade reports, and a resource is buried
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
in place — the OptiScaler fork in Jedi: Fallen Order — no queue is ever registered, the synchronous
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

When OptiScaler's layout bridge export `PeripheralWarpLayoutBridgeV1`
([`include/peripheral_warp/layout_bridge_v1.h`](../include/peripheral_warp/layout_bridge_v1.h)) is
found among the game's modules, the add-on forces OptiScaler's own spatial warp `Off` through the
bridge and keeps its own NGX hook on feature 18. The change is saved to `OptiScaler.ini`, so it
survives a restart. The whole pipeline — pack layout, temporal modes, filters, diagnostics — is then
ours, and the fork calls the model through `nvngx_dlssnr.dll` like any other host.

The tab reports `OptiScaler link: linked; OptiScaler's own warp is Off …` and offers the checkbox
**Warp inside OptiScaler's NR call**. `[PeripheralWarp] OptiScalerTakeover=0` restores the earlier
linked mode, in which OptiScaler performs the warp and the add-on's own features are unavailable;
there the consumer's layout is the truth (edits are forwarded with `Set`, a refusal restores its
values, and edits made in OptiScaler's menu are pulled with `Get` when its generation changes). The
bridge does not carry the center offset.

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
cleanly, and so are games the player kills. `CrashGuard=0` disables the guard.

## Remote tab for 32-bit games

In the DLSS 5 x86 kits a 32-bit game runs ReShade x86 with `dlss5-feed.addon32`, and Neural
Rendering runs in `host64\dlss5-feed-host64.exe`, where ReShade x64 loads RenoDX and
`optimizer-fps-dlss5.addon64`. Our tab would therefore be drawn into a window the player never sees.
`optimizer-fps-dlss5-remote.addon32`, placed next to `dlss5-feed.addon32`, draws the same tab in the game
and talks to the host through shared memory.

The protocol is [`adapters/reshade/pw_remote_ipc.h`](../adapters/reshade/pw_remote_ipc.h): a fixed
POD layout (offsets and size asserted, both sides built from the header) in the file mapping
`Local\PeripheralWarpRemoteV1`. One writer per direction, no locks — the overlay writes `settings`
and publishes by bumping `settingsGeneration` last; the host writes `status`, `applied` and
`hostHeartbeatTick` and publishes them with `statusGeneration`. Both sides copy whole sub-structs, so
a torn read is at worst one frame of a mixed value that the next generation corrects. Version 2 adds
the optical-flow fields; v1 is tolerated in both directions.

The host side runs on every present without a linked layout bridge and applies incoming settings
exactly as the local tab does, through the same setters and ini writers. The generation seen at
startup is adopted without applying, so a block left over from an earlier session never overrides
this process's ini.

The 32-bit side ([`adapters/reshade/producer_remote.cpp`](../adapters/reshade/producer_remote.cpp)) has no SDK core,
no D3D and no Detours — only the ReShade API and ImGui headers. Its banner is green, orange, or grey
(`host process not running (no shared block)`) when the mapping is missing or the heartbeat is older
than three seconds. The controls mirror the x64 tab and are seeded from `applied` when the host is
first seen. A note in the tab points out that compression in these kits may already be done by the
x86 feeder before the host sees the frame: if the Feeder build already compresses the frame, `Mode`
should be `Off` there, to avoid double compression. The temporal modes still run with `Mode` `Off`.

Build: `cmake --preset windows-x86-remote` then `cmake --build --preset windows-x86-remote-release`;
the output is `out/build/x86-remote/adapters/reshade/Release/optimizer-fps-dlss5-remote.addon32`. The
x64 add-on is unchanged for every other game: without a remote overlay the block is written and never
read.

## Source map

| | |
|---|---|
| `adapters/reshade/addon/` | the add-on shell: registration and `DllMain` (`addon_main.cpp`), the overlay, `[PeripheralWarp]` persistence and its ini schema, the OptiScaler layout bridge, the crash guard, the queue registration, the remote host side |
| `adapters/reshade/ngx/` | the NGX interposer: Detours and dispatch, feature lifetime and state, Pack/model/Unpack, the graveyard, the temporal machine and the background scheduler |
| `adapters/reshade/ngx_forwarder/` | `nvngx.dll_optimizerfps.dll` |
| `adapters/reshade/producer_remote.cpp` | the 32-bit remote tab, its own target |
| `adapters/reshade/diagnostics.h` | the `Debug*` switch block |
| `adapters/reshade/pw_remote_ipc.h`, `pw_ofa_cfg.h` | the shared-memory protocol; the host's optical-flow cfg |
| `shaders/temporal.hlsl` | the five temporal passes |

The per-file maps of those two folders are [dev/NGX_MODULES.md](dev/NGX_MODULES.md) and
[dev/ADDON_MODULES.md](dev/ADDON_MODULES.md).

## Deployment

The build deployed to games is the static-CRT one, `out/build/x64-mt` — see [BUILD.md](BUILD.md).
