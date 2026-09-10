# Optimizer FPS for DLSS5 — install guide

Optimizer FPS for DLSS5 is a ReShade add-on. It hooks DLSS 5 Neural Rendering (feature 18
of `nvngx_dlssnr.dll`) and compresses the periphery of the frame before the model sees it,
which reduces the model's input size. The central region of the screen stays at full
density.

It renders nothing by itself. Something else has to ask for neural rendering: RenoDX's
`renodx-dlss5`, DLSS5-Feeder, or an OptiScaler fork with `[DlssNr]`. Optimizer FPS sits
between that consumer and NVIDIA's runtime.

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
| Consumer | `renodx-dlss5`, DLSS5-Feeder (`host64`), or OptiScaler with `[DlssNr]` |
| Runtime | `nvngx_dlssnr.dll` 310.8 (the NR runtime) is what this release was tested against |

The installer checks two separate things. The **ReShade version** is read from the DLL's
file version resource and must be 6.8 or newer. **Add-on support** is detected from the
DLL's exports (`ReShadeRegisterAddon` / `ReShadeUnregisterAddon`), not from the version:
ReShade's plain build and its add-on build carry the same version number, and only the
exports decide whether an add-on can load at all.

## Install

Unpack the release zip anywhere and drag your game's `.exe` onto
`Install-OptimizerFPS.cmd`. Or, from a command prompt:

```
Install-OptimizerFPS.cmd "D:\Games\Baldurs Gate 3\bin\bg3_dx11.exe"
```

You can pass the game's folder instead of the exe when only one executable sits next to the
ReShade DLL. Then start the game; the settings are in the ReShade overlay (`Home`) under
**Add-ons → Optimizer FPS for DLSS5**.

## What the installer does

* Finds the ReShade DLL beside your game (`dxgi.dll`, `d3d11.dll`, `d3d12.dll`, `d3d9.dll`,
  `opengl32.dll`, `ReShade64.dll`, `ReShade32.dll`), reads its file version (6.8+ required)
  and checks its exports for add-on support.
* Copies `optimizer-fps-dlss5.addon64`, the NGX forwarder `nvngx.dll_optimizerfps.dll` and the
  compiled shaders in `optimizer-fps-dlss5\` next to it. Every copy is written to a temporary
  file, hashed against the payload manifest, and only then moved into place.
* Seeds `[PeripheralWarp]` in `ReShade.ini` with `Mode=2, CenterX=80, CenterY=80, WorkX=90,
  WorkY=90`, but **only** for keys that are not already there.
* Removes shaders from an older release that this one no longer ships (after copying them
  into the backup folder) and a stale `optimizer-fps-dlss5.session` crash-guard marker.
* Removes the files a release before 26.26 installed under the old names
  (`peripheral-warp.addon64`, `nvngx.dll_peripheralwarp.dll`, the `peripheral-warp\` folder,
  `peripheral-warp-remote.addon32`, `peripheral-warp.session`) once it has checked they are
  ours. They are copied into the receipt's backup folder first and listed in the receipt as
  `RemovedLegacyFiles`. A leftover old `.addon64` would otherwise be loaded a second time;
  `Verify-OptimizerFPS.ps1` warns while any of them is still there.
* Keeps a receipt (`_OptimizerFPS\latest-receipt.json` plus a timestamped backup folder with
  a copy of your `ReShade.ini`) so `-Mode Uninstall` can undo exactly what it did.

## What it never does

* It never downloads anything: everything it installs came out of the zip.
* It never touches your existing `[PeripheralWarp]` settings, your presets, your shaders, or
  any add-on that is not ours.
* It never overwrites a file that carries one of our names but is not ours; it stops and
  tells you, unless you pass `-Force` (which backs the file up first).
* It never installs ReShade, a neural-rendering consumer, or `nvngx_dlssnr.dll` for you.

<!-- README-END -->

## 32-bit games

A 32-bit process cannot load the 64-bit NGX runtime, so DLSS 5 Neural Rendering runs inside
DLSS5-Feeder's helper process, `<game>\host64\dlss5-feed-host64.exe`. Install DLSS5-Feeder
for that game first; then this installer:

* puts the add-on, the forwarder and the shaders into `host64\` (beside host64's own 64-bit
  ReShade), and
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

## Check that it works

```
Verify-OptimizerFPS.ps1 "D:\Games\Baldurs Gate 3\bin\bg3_dx11.exe"
```

The static half looks at the files, their hashes, the forwarder's exports, the shader count,
the ReShade build, `nvngx_dlssnr.dll`, the consumer, the `[PeripheralWarp]` section, the
`DisabledAddons` list and the crash-guard marker.

The runtime half reads `ReShade.log` beside ReShade (only if it is newer than the install;
otherwise it says "stale log") and reports:

| Line it looks for | What it means |
|---|---|
| `Registered add-on "Optimizer FPS for DLSS5"` | ReShade loaded the add-on |
| `nvngx_dlssnr.dll feature 18 create/evaluate/release hooked` | the NGX hook is in |
| `feature 18 created, native WxH, model WxH (…)` | the model's size, and why |
| `feature 18 adopted …` | the feature existed before the hooks went in |
| `first warped evaluate completed` | frames are actually being warped |
| `temporal mode N running` | a temporal mode is on |
| `background pass N adopted` | the model is running on its own queue |

When the log shows a warped frame, Verify prints the model's pixels as a percentage of the
native pixels. That number is the share of pixels the model processes, not the work saved.

## Update

```
Install-OptimizerFPS.cmd "…\game.exe" -Mode Update
```

The same as an install, except that it never rewrites a setting you already have. Files that
already match the payload are reported as "up to date" and left alone; files from an older
build of ours are adopted and replaced without a backup, since they were ours to begin with.

## Uninstall

```
Install-OptimizerFPS.cmd "…\game.exe" -Mode Uninstall
```

Reads the receipt and removes only the files whose current hash is still the one it
installed; anything you changed by hand is left in place and reported. Originals that it had
backed up are restored, and only the ini keys it wrote are removed (the `[PeripheralWarp]`
section goes too if that empties it). The backup folders stay.

The migrated `DisabledAddons` name is deliberately **not** reverted: the old, versioned name
is one no current build answers to.

## Exit codes

| Code | Meaning |
|---|---|
| 0 | done |
| 1 | failed |
| 2 | the game (or `dlss5-feed-host64.exe`) is running |
| 3 | no ReShade found beside the game |
| 4 | ReShade too old, or a build without add-on support |
| 5 | 32-bit game without `host64` |
| 6 | the payload is corrupt (does not match `files.sha256`) |
| 7 | nothing to uninstall |
| 10 | installed, but the game has not run with it yet |

## Options

| | |
|---|---|
| `-Mode Install\|Update\|Verify\|Uninstall` | what to do (default `Install`) |
| `-Payload <dir>` | where the payload is (default `payload\` beside the script) |
| `-Yes` | answer every prompt with yes |
| `-NoPause` | do not wait for Enter at the end |
| `-NoVerify` | skip the check at the end of an install |
| `-Force` | replace a foreign file that carries one of our names (backing it up) |
| `-NoIni` | do not touch `ReShade.ini` at all |
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
installer (it removes the marker). `CrashGuard=0` in `[PeripheralWarp]` turns the guard off.

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

| From the payload | Goes to (64-bit game) | Goes to (32-bit game) |
|---|---|---|
| `x64\optimizer-fps-dlss5.addon64` | beside the ReShade DLL | `<game>\host64\` |
| `x64\nvngx.dll_optimizerfps.dll` | beside the add-on | `<game>\host64\` |
| `x64\optimizer-fps-dlss5\*.dxbc` | `optimizer-fps-dlss5\` beside the add-on | `<game>\host64\optimizer-fps-dlss5\` |
| `x86\optimizer-fps-dlss5-remote.addon32` | — | beside the game's 32-bit ReShade DLL |

If `ReShade.ini` has `[ADDON] AddonPath=`, "beside the add-on" means that folder instead.
Unblock every file, delete any leftover `optimizer-fps-dlss5.session`, and add this to
`ReShade.ini`:

```
[PeripheralWarp]
Mode=2
CenterX=80
CenterY=80
WorkX=90
WorkY=90
```

For a 32-bit game whose Feeder build already compresses the frame, use `Mode=0` instead to
avoid double compression.

Running the installer afterwards is still safe: it recognizes its own files by their exports
(`PeripheralWarpSetLayoutV1` / `PeripheralWarpSetTemporalV1` for the add-on,
`pw_ngx_call_*` for the forwarder), adopts them, and writes a receipt.
