# Troubleshooting

Every case below has actually happened. Start with the add-on's tab: its first line says whether the
frame the model sees is being warped right now and, if not, why.

Before anything else, run the verifier. It checks the files, the hashes, the ReShade build, the
consumer, the ini, and the log in one go:

```powershell
.\Verify-OptimizerFPS.ps1 -GameExe 'D:\Games\Example\game.exe' -Json
```

Run it after installation and again after a new game session. A log older than
the receipt's install time is stale and cannot prove the current core loaded.
Exit code 10 means the files passed static checks but the successful runtime
load has not been verified yet. Exit code 1 means a check failed.

## Core load failed or version mismatch

The x64 add-on needs `optimizer-fps-dlss5-core.dll` beside it, with the shader
folder `optimizer-fps-dlss5\` in the same directory. For a 32-bit kit, those
files belong in the 64-bit host's add-on directory, not beside the game. If
`[ADDON] AddonPath` selects a different directory, keep the entire x64 set there.

Verify checks the core's file format, exports, version, and installed hash.
The package manifest or install receipt supplies the trusted version and hash:
this is the static check of the exact core file. The exports alone do not prove
which ABI the DLL returns. If a local version helper is unavailable, Verify
marks the runtime ABI as unverified until a fresh `ReShade.log` reports the
loaded ABI and release.
Its JSON has `Static.Core` for those checks and `Runtime.Core` for the latest
fresh log verdict. `Core load failed` may mean a missing DLL, a different
release already loaded in the same process, or an ABI mismatch. Reinstall the
matching add-on, core, forwarder, and shaders from one zip, then start a new
session and run Verify again. A hash or version mismatch is a failure, even if
an older log says the core once loaded. If the installed files check out but
the trusted reference or load result is unknown, inspect the new `ReShade.log`; do not treat
an old successful line as evidence for the current installation.

## The add-on is not in ReShade's Add-ons list

`[ADDON] DisabledAddons` in `ReShade.ini` contains its name. Older releases carried the release
number in the exported name, so a "disabled" choice was recorded against something like
`Optimizer FPS for DLSS5 26.15`; the installer deliberately migrates that entry to the current
version-free name, so a disabled add-on does not silently re-enable itself. Turn it back on under
**Home → Add-ons**.

If the list has no entry at all, ReShade never loaded the file. Check that ReShade is a 6.8+ build
**with full add-on support** (the plain build carries the same version number but no
`ReShadeRegisterAddon` export) and that the file is not blocked by the mark of the web: right-click
the zip → Properties → Unblock **before** unpacking.

## The tab says NOT ACTIVE

The reason in the banner tells you which of these it is.

### `crash guard: the previous session … ended right after the first warped frame`

The marker file `optimizer-fps-dlss5.session` beside the add-on is written just before the first warped
evaluate and removed on a clean unload. It was still there at startup, so this session forwards
every NR call untouched.

Three ways out, in order of convenience:

1. Press **Retry warping now** in the tab; it re-enables the warp immediately. In a 32-bit game the
   button is in the game's own tab (Optimizer FPS for DLSS5, tab for the 64-bit host).
2. Delete `optimizer-fps-dlss5.session` beside the add-on (re-running the installer does it too). In a
   32-bit game it is in `host64\`.
3. `[OptimizerFPS] CrashGuard=0` in `ReShade.ini` turns the guard off entirely.

A session that ran for more than 20 s after its first warped frame is not counted as a crash. That is
why Feeder's `host64`, which is killed at game exit and never unloaded cleanly, does not trip it. Since
2026.10 a `host64` that the game stops on purpose (settings applied, the host restarted, the game closed)
does not trip it either, however soon that happens: the game's tab sees the host end with exit code 0
and removes the marker. That needs the 2026.10 tab (`optimizer-fps-dlss5-remote.addon32`) in the game.

### `nvngx_dlssnr.dll is not loaded in this process`

Nothing in this process does neural rendering. Either the NR consumer is not installed, or the game
is 32-bit and NR lives in `host64` (see below).

### `waiting for the host to create feature 18`

The consumer is there but has not asked for NR. In RenoDX, check that neural rendering is enabled in
its menu (`NeuralUplift`); in a game that simply does not use NR, there is nothing to accelerate.

The hook itself is fine at this point. If the banner instead shows `hook attempts so far: N`, the
Detours hook has not landed yet; it is retried every frame, and hosts that create their feature
during a level load can take a while.

### `mode is Off`, or `the layout does not reduce the frame (work size equals native)`

Set **Mode** to `Peripheral` and Work below 100 %, or lower Global scale. With Mode `Off` the add-on
still drives the temporal cadence on the native model, so this is a valid configuration; it just does
not warp.

### `host supplied no color/depth/motion/output`, `unsupported resource format`

A pass-through: rather than guess, the add-on refused to touch a frame it could not handle correctly.
The exact text names the input that failed. These cases are consumer- and game-specific; report them
with the `ReShade.log` line.

### `the host upscales inside NR …`, `host colour region is …`, `host output region is …`

The consumer hands the model a frame the compression cannot take: a model that upscales (its output is
larger than its input), a colour region that changes with dynamic resolution, or an output region that
does not start at the top-left of its texture. An output texture that is only *larger* than the frame
is fine as long as the frame sits at its top-left. The model then runs untouched, and the temporal
modes and extra passes are off too, because they assume colour and output of the same size. The
verdict holds until you change the layout in the tab. The `ReShade.log` line lists the extents and
regions the consumer gave the model; attach it to a report.

## A 32-bit game shows no tab

There are two add-ons for a 32-bit game and they do different jobs:

* `optimizer-fps-dlss5.addon64`, `optimizer-fps-dlss5-core.dll`, the forwarder,
  and the shader folder run in `<game>\host64\`, beside host64's own 64-bit ReShade.
  That is where the work happens.
* `optimizer-fps-dlss5-remote.addon32` sits beside the game's 32-bit ReShade and draws the same tab in
  the game, talking to the host over shared memory.

If the game's overlay has no tab, the remote add-on is missing or disabled there. If it has a tab
but the banner is gray (`host process not running (no shared block)`), then `host64` is not running
or its own ReShade never loaded `optimizer-fps-dlss5.addon64`; check the ReShade log inside `host64\`.

Some Feeder builds already compress the frame before `host64` sees it. If yours does, set **Mode** to
`Off` to avoid double compression; the installer seeds `Mode=2` there exactly as it does for a 64-bit
game. The temporal modes still run with Mode `Off`, on the native model.

## Two NR consumers in one game

Only one of them gets a compressed frame. In Control, where `renodx-control-rr` and `renodx-dlss5`
both ask for feature 18, the second gets `0xBAD00002` from the model's own caller check while the
first keeps working, warped. Nothing crashes; this is expected, not a bug to fix here.

## "shaders were not found in optimizer-fps-dlss5\"

The `optimizer-fps-dlss5\` folder with the `.dxbc` files must sit next to the `.addon64`, not next to the
game's executable. If `ReShade.ini` sets `[ADDON] AddonPath`, "next to the add-on" means *that*
folder; the installer follows it. Re-running the installer fixes both cases.

A related one: if only the temporal modes fail, with `temporal_*.dxbc missing in optimizer-fps-dlss5\`,
the folder is from an older release and lacks a shader this build needs.

## "nvngx.dll_optimizerfps.dll is missing" / "lacks its exports"

The forwarder is what actually calls NVIDIA's signed snippet, which only accepts calls from a module
with `nvngx.dll` in its name. It must sit beside the `.addon64` and export `pw_ngx_call_create`,
`pw_ngx_call_evaluate`, and `pw_ngx_call_release`. Re-run the installer.

## Files with the old `peripheral-warp` names are still there

Up to 26.25 the shipped files were called `peripheral-warp.addon64`,
`nvngx.dll_peripheralwarp.dll`, `peripheral-warp-remote.addon32`, the shader folder was
`peripheral-warp\` and the crash-guard marker `peripheral-warp.session`. From 26.26 they carry the
product's name instead. An old `.addon64` left beside the new one is loaded a second time by
ReShade, so it has to go.

Run the installer once (`-Mode Update`): it checks that the old-name files are ours, copies them
into the receipt's backup folder, deletes them, removes the empty `peripheral-warp\` folder and
records the lot in `RemovedLegacyFiles` in the receipt. `Verify-OptimizerFPS.ps1` warns as long as
any of them is still there. A file with an old name that is *not* ours is never touched.

## Windows Defender removed a file

The add-on uses Microsoft Detours to hook DLSS functions, which some antivirus heuristics flag. The
installer never adds an exclusion by itself: it names the file that was removed and stops. If you
trust this download, add an exclusion for that file (only that file, not the folder) under
Settings → Virus & threat protection → Exclusions, then re-run the installer to restore whatever
was quarantined. `Install-OptimizerFPS.cmd "…\game.exe" -AllowDefenderExclusion` asks once and adds
it for you; `-Mode Uninstall` removes it again.

## Background mode silently behaves like the synchronous one

The tab and the log say `background mode unavailable here (no registered host queue); running the
synchronous interpolation`. The background pass needs a D3D12 queue that ReShade reported through
`init_command_queue`; when the consumer creates its device before ReShade is in place — always the
case when OptiScaler loads ReShade (`LoadReshade=true`) — no queue is ever registered. Use
**Interpolate** (sync) there.

## The picture looks compressed twice with OptiScaler

Some OptiScaler builds have their own peripheral compression (wilsjo2
OptiScaler-DLSSNR-PreSR-Multipass v0.8.9 and later, `[DlssNr] SpatialCompression`). With it on,
the model already receives a packed frame and this add-on packs it again: the edges look softer and
the centre is no longer 1:1. Turn one of them off — their `SpatialCompression`, or **Mode** `Off`
in this add-on's tab.

## Nothing happens with OptiScaler

Check `ReShade.log`: the add-on must be registered at all, which needs `[Plugins] LoadReshade=true`
in `OptiScaler.ini` and ReShade **with add-on support** beside OptiScaler as `ReShade64.dll`. Then
look for `feature 18 adopted` or `feature 18 created`; if neither appears, OptiScaler's own NR is
off (`[DlssNr] Enabled`) or not running in this game — check it without the add-on first.

## Colors look wrong

**The warped frame is slightly brighter than the native one.** The model's own tone processing
depends on the frame scale; Pack and Unpack are linear averages and add no gain. Compensate by eye
with **Brightness** and **Gamma** under "Output colour"; they apply to warped frames only. Lowering
the consumer's Local Tone / Intensity reduces the shift at the source. Because the adjustment is
applied before the host decodes the model's output, its effect on the final picture is not 1:1 with
the number.

**Death Stranding with RenoDX.** The game's 10-bit SDR buffer is treated as HDR10 by RenoDX's codec.
That happens outside this add-on and cannot be fixed here.

**Alan Wake 2: NR appears to do nothing with Ray Reconstruction on.** RenoDX applies it pre-post, so
the effect is invisible. This happens in the consumer's rendering pipeline, not in the add-on.

## Crashes in a game with a heavy third-party stack

Cyberpunk-style setups with many overlays and injectors can crash for reasons that have nothing to
do with this add-on. To find out whether it is involved, set `[OptimizerFPS] Passive=1` in
`ReShade.ini`: the add-on is loaded and registered and does nothing else, with no hooks, no events,
and no tab (its entry stays in ReShade's Add-ons list, but there is nothing to open). If the crash
persists, test with the add-on removed, then isolate the remaining mods one at a time. Remove the key
to get the tab back.

## Reporting something

Attach `ReShade.log` (the run that shows the problem), the `[OptimizerFPS]` section of your
`ReShade.ini`, the `Verify-OptimizerFPS.ps1 -Json` output, and which NR consumer plus which
`nvngx_dlssnr.dll` version you are on. The log lines that matter most are
`Registered add-on "Optimizer FPS for DLSS5"`, `feature 18 create/evaluate/release hooked`,
`feature 18 created, native WxH, model WxH`, and `first warped evaluate completed`.

For deeper digging there are diagnostic `Debug*` keys in `[OptimizerFPS]`. They are listed in
[RESHADE_ADDON.md](RESHADE_ADDON.md) and read once at add-on load, so a change needs a restart.
