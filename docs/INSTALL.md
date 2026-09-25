# Optimizer FPS for DLSS5 — install guide

Optimizer FPS for DLSS5 is a ReShade add-on. It hooks DLSS 5 Neural Rendering (feature 18
of `nvngx_dlssnr.dll`) and compresses the periphery of the frame before the model sees it,
which reduces the model's input size. The central region of the screen stays at full
density.

It renders nothing by itself. Something else has to ask for neural rendering: RenoDX's
`renodx-dlss5`, DLSS5-Feeder, or a public OptiScaler build with Neural Rendering that loads
ReShade (see [With a public OptiScaler](#with-a-public-optiscaler)). Optimizer FPS sits
between that consumer and NVIDIA's runtime.

The release zip includes the installer, its `installer/` modules, and a `payload/`
folder. Keep the add-on, core DLL, forwarder, and shaders from the same zip;
`payload/files.sha256` lists the exact files in each package.

## Before you install

The add-on hooks DLSS functions inside the game process. Do **not** use it in games with
anti-cheat (Easy Anti-Cheat, BattlEye, Vanguard, Ricochet) or in online multiplayer; you
may get banned. Use at your own risk.

To disable the add-on, uncheck it in ReShade's **Add-ons** tab (or remove the files) and
restart the game; the hooks stay in place until the process exits. `-Mode Uninstall`
removes the files for you.

Optimizer FPS is not affiliated with NVIDIA, ReShade, or RenoDX. DLSS, NGX, and GeForce are
trademarks of NVIDIA Corporation.

## Support

Bugs: open a GitHub Issue and attach `ReShade.log` and the output of
`Verify-OptimizerFPS.ps1 -Json`.

## License

Optimizer FPS is MIT-licensed and provided as is, with no warranty. NVIDIA runtimes and the
ReShade/RenoDX applications are not bundled; the ReShade add-on API headers (BSD-3) and
Dear ImGui / Microsoft Detours (MIT) are used under their licenses (see NOTICE). Install
NVIDIA's runtime and your neural-rendering consumer from their own sources.

## Before you start

| | |
|---|---|
| Windows | 10 or 11, 64-bit |
| GPU | NVIDIA RTX 20-series or newer (developed and tested on an RTX 4080 SUPER) |
| Driver | one that ships DLSS 5 Neural Rendering |
| ReShade | **6.8 or newer, the build "with full add-on support"** |
| Consumer | `renodx-dlss5`, DLSS5-Feeder (`host64`), or a public OptiScaler with NR and `LoadReshade=true` |
| Runtime | `nvngx_dlssnr.dll` 310.8 (the NR runtime) is what this release was tested against |

The installer checks two separate things. The **ReShade version** is read from the DLL's
file version resource and must be 6.8 or newer. **Add-on support** is detected from the
DLL's exports (`ReShadeRegisterAddon` / `ReShadeUnregisterAddon`), not from the version:
ReShade's plain build and its add-on build carry the same version number, and only the
exports decide whether an add-on can load at all.

## Install

Unpack the release zip anywhere and drag your game's `.exe` onto
`Install-OptimizerFPS.cmd`. Or, from the unpacked folder, run:

```cmd
Install-OptimizerFPS.cmd "D:\Games\Baldurs Gate 3\bin\bg3_dx11.exe" -NoPause
```

You can pass the game's folder instead of the exe when only one executable sits next to the
ReShade DLL. Then start the game; the settings are in the ReShade overlay (`Home`) under
**Add-ons → Optimizer FPS for DLSS5**.

PowerShell offers the same installer options:

```powershell
.\Install-OptimizerFPS.ps1 -GameExe 'D:\Games\Baldurs Gate 3\bin\bg3_dx11.exe' -Mode Install
```

## What the installer does

* Finds the ReShade DLL beside your game (`dxgi.dll`, `d3d11.dll`, `d3d12.dll`, `d3d9.dll`,
  `opengl32.dll`, `ReShade64.dll`, `ReShade32.dll`), reads its file version (6.8+ required)
  and checks its exports for add-on support.
* Copies `optimizer-fps-dlss5.addon64`, `optimizer-fps-dlss5-core.dll`, the NGX forwarder
  `nvngx.dll_optimizerfps.dll`, and the compiled shaders in `optimizer-fps-dlss5\` next to
  the add-on. Every copy is written to a temporary
  file, hashed against the payload manifest, and only then moved into place.
* Seeds `[OptimizerFPS]` in `ReShade.ini` with `Mode=2, CenterX=80, CenterY=80,
  WorkX=90, WorkY=90` when neither the current nor the older section exists.
  `-Mode Update` migrates an older section as described below.
* Removes shaders from an older release that this one no longer ships (after copying them
  into the backup folder) and a stale `optimizer-fps-dlss5.session` crash-guard marker.
* Removes the files a release before 26.26 installed under the old names
  (`peripheral-warp.addon64`, `nvngx.dll_peripheralwarp.dll`, the `peripheral-warp\` folder,
  `peripheral-warp-remote.addon32`, `peripheral-warp.session`) once it has checked they are
  ours. They are copied into the receipt's backup folder first and listed in the receipt as
  `RemovedLegacyFiles`. A leftover old `.addon64` would otherwise be loaded a second time;
  `Verify-OptimizerFPS.ps1` warns while any of them is still there.
* Keeps a Schema 3 receipt (`_OptimizerFPS\latest-receipt.json` plus a timestamped backup
  folder with copies of affected `ReShade.ini` files). It records the core path, hash,
  version, and installer-owned keys so `-Mode Uninstall` can undo its own changes.

## What it never does

* It never downloads anything: everything it installs came out of the zip.
* It does not rewrite your existing settings during `Update`. It migrates the older
  section only when the current section is absent; your presets and unrelated files stay intact.
* It never overwrites a file that carries one of our names but is not ours; it stops and
  tells you, unless you pass `-Force` (which backs the file up first).
* It never installs ReShade, a neural-rendering consumer, or `nvngx_dlssnr.dll` for you.

<!-- README-END -->

## 32-bit games

A 32-bit process cannot load the 64-bit NGX runtime, so DLSS 5 Neural Rendering runs inside
DLSS5-Feeder's helper process, `<game>\host64\dlss5-feed-host64.exe`. Install DLSS5-Feeder
for that game first; then this installer:

* puts the add-on, core DLL, forwarder, and shaders into `host64\` (beside host64's own
  64-bit ReShade), and
* puts `optimizer-fps-dlss5-remote.addon32` beside the game's own 32-bit ReShade DLL. That is
  only a tab: it shows and edits the host's settings from inside the game's overlay.

If `host64\dlss5-feed-host64.exe` is not there, the installer stops with exit code 5 and
points you at the DLSS5-Feeder installer. It also warns if `dlss5-feed.addon32` is missing
beside the 32-bit ReShade, because without it the game never hands its frames to host64.

**Set Mode to `Off` if the Feeder already compresses the frame.** The installer seeds
`Mode=2` (Peripheral) exactly as it does for a 64-bit game, but some Feeder builds already
compress the frame before `host64` sees it. In that case set **Mode** to `Off` in the tab to
avoid double compression; the temporal modes still work with Mode `Off`, because they run on
the native model.

## With a public OptiScaler

OptiScaler builds with Neural Rendering run NR themselves and can load ReShade, which then loads
this add-on. Tested: [Dagherbou/OptiScaler_DLSSNR](https://github.com/Dagherbou/OptiScaler_DLSSNR)
`v0.2.0-patch1` and
[wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass](https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass)
`v0.8.3` and `v0.8.91`, on D3D12 and on D3D11 through OptiScaler's D3D11-on-D3D12 bridge.

1. Install OptiScaler with NR by its own instructions and check that NR works without this add-on.
2. Put ReShade 6.8+ **with add-on support** beside OptiScaler as `ReShade64.dll` (ReShade's own
   setup names it after the API; rename it, or copy the DLL from the add-on build).
3. In `OptiScaler.ini`, set `[Plugins] LoadReshade=true`.
4. Run this installer on the game's exe; it finds `ReShade64.dll` like any other ReShade DLL.

The add-on's settings open from OptiScaler's menu key as well (Insert by default): a window with
them appears beside OptiScaler's menu. ReShade's own overlay (Home) has the same settings in its
**Add-ons** tab.

In the game, the add-on's tab shows `ACTIVE` once OptiScaler's model runs; the log says
`feature 18 adopted` (OptiScaler created the model before ReShade loaded) and then
`first warped evaluate completed`.

* **One compression only.** wilsjo2 v0.8.9 and later has its own peripheral compression
  (`[DlssNr] SpatialCompression`, off by default). With both on, the frame is compressed twice.
  Keep theirs off, or set this add-on's **Mode** to `Off`. The add-on's tab warns in red when
  `SpatialCompression` is on in `OptiScaler.ini`.
* **NR before the upscaler** (wilsjo2 `RunBeforeSR=true`) works: the add-on compresses the
  render-resolution frame the model receives.
* **Frame generation** comes from OptiScaler itself (`[FrameGen]`, for example `FGInput=upscaler`,
  `FGOutput=dlssg`); the add-on works alongside it.
* **Background mode** runs as the synchronous one here: OptiScaler loads ReShade after the game's
  D3D12 queue exists, so the add-on never sees that queue. The tab says so.

## Check that it works

```powershell
.\Verify-OptimizerFPS.ps1 -GameExe 'D:\Games\Baldurs Gate 3\bin\bg3_dx11.exe' -Json
```

You can also use the installer entry point with `-Mode Verify`:

```cmd
Install-OptimizerFPS.cmd "D:\Games\Baldurs Gate 3\bin\bg3_dx11.exe" -Mode Verify -Json -NoPause
```

The static check covers the manifest and installed hashes, the core DLL's x64 format,
exports and version, the forwarder, all listed shaders, the ReShade build, the
consumer, `[OptimizerFPS]`, `DisabledAddons`, and the crash-guard marker. An older
`[PeripheralWarp]` section is a migration warning, not a current configuration.

The runtime half reads `ReShade.log` beside ReShade (only if it is newer than the install;
otherwise it says "stale log") and reports:

| Line it looks for | What it means |
|---|---|
| `Registered add-on "Optimizer FPS for DLSS5"` | ReShade loaded the add-on |
| `Optimizer FPS: core loaded; ABI 1; release ...` | the current core loaded successfully |
| `Optimizer FPS: core load failed: ...` | the core did not load; check the reason and installed version |
| `nvngx_dlssnr.dll feature 18 create/evaluate/release hooked` | the NGX hook is in |
| `feature 18 created, native WxH, model WxH (…)` | the model's size, and why |
| `feature 18 adopted …` | the feature existed before the hooks went in |
| `first warped evaluate completed` | frames are actually being warped |
| `temporal mode N running` | a temporal mode is on |
| `background pass N adopted` | the model is running on its own queue |

The log must be newer than the receipt's install time. An older log cannot prove this
installation loaded. Before a fresh run, Verify returns 10 for an otherwise valid
installation; a static or recorded core failure returns 1. A fresh successful core load
can return 0 even when no frame was warped. When the log shows a warped frame, Verify
prints the model's pixels as a percentage of native pixels, not the work saved.

## Update

```cmd
Install-OptimizerFPS.cmd "D:\Games\Example\game.exe" -Mode Update -NoPause
```

Update keeps existing settings. If only `[PeripheralWarp]` exists, it copies known
settings into `[OptimizerFPS]` and removes the old section. If `[OptimizerFPS]` already
exists, that section wins, even when a value is empty; Update removes the old section
without mixing values. Unknown keys in the old section are not copied. The installer
backs up affected ini files before writing them. In a 32-bit installation it checks and
backs up both the game's and the host's ini files before changing either one.

Files that match the payload stay in place. Files from an older build of this add-on
are adopted and replaced. The Schema 3 receipt transfers ownership only for unchanged
keys the installer wrote; user-edited values remain yours. The `DisabledAddons` name
migration remains in place so an add-on you disabled does not silently turn on again.

## Uninstall

```cmd
Install-OptimizerFPS.cmd "D:\Games\Example\game.exe" -Mode Uninstall -NoPause
```

Reads the receipt and removes only the files whose current hash is still the one it
installed; anything you changed by hand is left in place and reported. Originals that it had
backed up are restored, and only unchanged ini keys recorded as installer-owned are
removed (the `[OptimizerFPS]` section goes too if that empties it). User-edited keys
remain. The backup folders stay.

The migrated `DisabledAddons` name is deliberately **not** reverted: the old, versioned name
is one no current build answers to.

## Exit codes

| Code | Meaning |
|---|---|
| 0 | completed; for Verify, the files and a fresh core load check out |
| 1 | failed; for Verify, a static or fresh runtime failure was found |
| 2 | the game (or `dlss5-feed-host64.exe`) is running |
| 3 | no ReShade found beside the game |
| 4 | ReShade too old, or a build without add-on support |
| 5 | 32-bit game without `host64` |
| 6 | the payload is corrupt (does not match `files.sha256`) |
| 7 | nothing to uninstall |
| 10 | installed, but a fresh successful runtime check is not available yet |

## Options

| | |
|---|---|
| `-Mode Install\|Update\|Verify\|Uninstall` | what to do (default `Install`) |
| `-Payload <dir>` | where the payload is (default `payload\` beside the script) |
| `-Yes` | answer every prompt with yes |
| `-NoPause` | do not wait for Enter at the end |
| `-NoVerify` | skip the check at the end of an install |
| `-Force` | replace a foreign file that carries one of our names (backing it up) |
| `-NoIni` | leave `ReShade.ini` untouched, including during Update or Uninstall; file and receipt operations still run |
| `-Json` | print a machine-readable summary instead of the pretty output |
| `-AllowDefenderExclusion` | allow the installer to offer a Defender exclusion for a file Defender removed (never implied by `-Yes`) |

## Troubleshooting

**SmartScreen / "this file came from another computer".** Files unpacked from a downloaded
zip carry the mark of the web, and ReShade will not load them. The installer runs
`Unblock-File` on everything it writes; if you copied files by hand, right-click the zip →
Properties → Unblock **before** unpacking.

**Windows Defender removed a file.** The add-on uses Microsoft Detours to hook DLSS
functions, which some antivirus heuristics flag. The installer never adds an exclusion on its
own: it tells you which file was removed and stops. If you trust this download, add an
exclusion for that file (only that file, not the folder) under Settings → Virus & threat
protection → Exclusions, or re-run the installer with `-AllowDefenderExclusion`, which asks
once and adds it for you (one UAC prompt). `-Mode Uninstall` removes an exclusion the installer
added.

**The add-on is in the overlay but does nothing.** Check `[ADDON] DisabledAddons` in
`ReShade.ini`. Older releases of this add-on carried the release number in their name, so a
"disabled" choice was recorded against a name like `Optimizer FPS for DLSS5 26.15`. The
installer deliberately carries that state over to the new, version-free name, so a disabled
add-on does not silently re-enable itself. Enable it under **Home → Add-ons**.

**Everything is forwarded untouched after a crash.** The crash guard writes
`optimizer-fps-dlss5.session` beside the add-on the moment the first warped frame is recorded. If
the game dies right after that, the marker is still there next session and the add-on starts
in pass-through on purpose. Press **Retry** in the add-on tab, delete the file, or re-run the
installer (it removes the marker). `CrashGuard=0` in `[OptimizerFPS]` turns the guard off.

**"shaders were not found in optimizer-fps-dlss5\".** The `optimizer-fps-dlss5\` folder has to sit
next to the `.addon64`, not next to the game. If `ReShade.ini` sets `[ADDON] AddonPath`,
"next to the add-on" means that folder — the installer follows it.

**`nvngx.dll_optimizerfps.dll is missing` / `lacks its exports`.** The forwarder is what
calls NVIDIA's signed snippet; it must sit beside the `.addon64` and export
`pw_ngx_call_create`, `pw_ngx_call_evaluate`, `pw_ngx_call_release`. Re-run the installer.

**No `feature 18` in the log at all.** Nothing asked for neural rendering that session:
check that your consumer is installed and enabled, and that DLSS 5 NR is actually on in the
game.

## Manual install

If you would rather place the files yourself:

| In the zip / payload | 64-bit game: beside x64 ReShade or in its `[ADDON] AddonPath` | 32-bit kit |
|---|---|---|
| `payload\VERSION.txt`, `payload\files.sha256` | package metadata used by the installer and Verify | same package metadata |
| `payload\x64\optimizer-fps-dlss5.addon64` | add-on directory | `<game>\host64\optimizer-fps-dlss5.addon64` |
| `payload\x64\optimizer-fps-dlss5-core.dll` | beside the add-on | `<game>\host64\optimizer-fps-dlss5-core.dll` |
| `payload\x64\nvngx.dll_optimizerfps.dll` | beside the add-on | beside the add-on in `host64\` |
| `payload\x64\optimizer-fps-dlss5\*.dxbc` | `optimizer-fps-dlss5\` beside the add-on | `<game>\host64\optimizer-fps-dlss5\*.dxbc` |
| `payload\x86\optimizer-fps-dlss5-remote.addon32` | unused | beside the game's 32-bit ReShade DLL |

For a 32-bit kit, `[ADDON] AddonPath` in the host's `ReShade.ini` moves the x64
add-on, core, forwarder, and shader folder together into that selected host add-on
directory. The remote add-on uses the game's selected add-on directory. Never put
the x64 core DLL beside the 32-bit game executable.

Unblock every file, delete any leftover `optimizer-fps-dlss5.session`, and add this to
`ReShade.ini`:

```ini
[OptimizerFPS]
Mode=2
CenterX=80
CenterY=80
WorkX=90
WorkY=90
```

For a 32-bit game whose Feeder build already compresses the frame, use `Mode=0` instead to
avoid double compression.

Running the installer afterwards recognizes the add-on by its current setting export
(`OptimizerFpsSetSettingV1`) or supported older aliases, and recognizes the core by
its `OfpsCoreVersion` and `OfpsCreateCore` exports. It checks the package hashes and
writes a receipt.
