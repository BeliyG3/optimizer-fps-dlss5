# Plan 8 task 1: the add-on on top of a public, unpatched OptiScaler

Result: **PASS** on both stands (2026-09-25). The add-on works as a plain ReShade add-on inside a
public OptiScaler build: OptiScaler runs its own DLSS Neural Rendering and loads ReShade
(`[Plugins] LoadReshade=true`), ReShade loads the add-on, and the add-on takes OptiScaler's NR
model (feature 18) and warps the frame around it. No OptiScaler patch is involved.

## What was tested

- **OptiScaler:** Dagherbou OptiScaler_DLSSNR, tag `v0.2.0-patch1` (`97376162`), built clean in
  a separate local folder (a clone of the upstream repository at that tag; the
  submodules were copied from a checkout at the same commits, nothing was downloaded).
  `OptiScaler.dll` SHA-256 `6169213E1BC3108297CF52DB0A2EACD4A1408F086947094E1332BF04DE23CB0A`.
  Its stock `OptiScaler.ini` with `[DlssNr] Enabled=true`, `[Plugins] LoadReshade`, and for the
  D3D11 stand `[Upscalers] Dx11Upscaler=dlss_12`.
- **Add-on:** current `out/build/x64` add-on, core, forwarder and DXBC; ReShade 6.8 add-on build
  as `ReShade64.dll`; `[OptimizerFPS]` in `ReShade.ini`.
- **Stands:** bench12 (native D3D12, DLSS Ray Reconstruction through NGX, 1920×1080 — like
  007 First Light) and pw_bench (D3D11 through OptiScaler's D3D11-on-D3D12 bridge, DLAA, 3840×2160
  — like Fallen Order). Script: `tools/bench12/check_plan8_public_optiscaler.py`.

## Results

| Check | D3D12 (bench12) | D3D11 via bridge (pw_bench) |
|---|---|---|
| Add-on hooks the NR snippet and takes OptiScaler's model | adopted at 1920×1080, re-created at 1728×972 | adopted at 3840×2160 |
| Add-on loaded with Mode 0 changes nothing | MAD 0.0 against OptiScaler alone | MAD 0.0 |
| Warp active (Mode 2) | warped; MAD 2.62 / 1.63 against OptiScaler alone | warped; MAD 0.60 / 1.18 |
| Compute against pixel path | MAD 0.0 (both paths ran: "compute path ready" / "pixel path forced") | MAD 0.0 |
| Determinism (two warped runs) | frames byte-identical | not run |
| Frame carry, mode 1 | running | running |
| Frame carry, mode 3 (model in the background) | **runs as mode 1**, see below | not run |
| State sentinel (next draw/dispatch after our work) | PASS in every case | no sentinel on this stand |
| Device removal, exceptions, crash guard | none | none |

MAD values are on frames 120 / 239, RGB8.

## Limitation found

With ReShade loaded by OptiScaler, the game's D3D12 device and queue exist before ReShade, so
ReShade never reports the host queue to the add-on. Background mode (TemporalMode 3) needs that
queue; the add-on falls back to the synchronous carry and logs: "background mode needs the host's
D3D12 queue and none is registered in this process (device created before ReShade loaded); the
synchronous interpolation runs instead". The picture is correct; only the frame-time benefit of
mode 3 is missing. Goes to `docs/LIMITATIONS.md` (plan 8 task 4).

## Second public build: wilsjo2 OptiScaler-DLSSNR-PreSR-Multipass

Downloaded with the owner's permission (release archives, SHA-256 checked against the published
values): `v0.8.3` (latest stable, 13.09, `OptiScaler.dll` SHA-256 `856F14F1…A16`) and `v0.8.91`
(prerelease, 23.09, `42B3B7C7…778`). The build creates the NR model through NGX, and after a driver
refusal through its own "direct compatibility runtime"; both paths end in the `nvngx_dlssnr.dll`
exports the add-on hooks. Stand: the same script with `--optiscaler <release folder>`, 17 cases.

**Both versions: PASS.** Same picture as with Dagherbou (Mode 0 unchanged, warp active, compute =
pixel, carry mode 1, background mode falls back to synchronous, state sentinel PASS, D3D11 through
the bridge works). The D3D11 stand showed MAD 0.20–0.24 on frame 239 once with v0.8.3 and 0.0 with
v0.8.91 for the same cases: stand variance, not the add-on.

Specific to this build:

- **NR before the upscaler** (`[DlssNr] RunBeforeSR=true`, their main mode): the model runs on the
  render-resolution frame (960×540 on the stand); the add-on takes it and warps it to 864×486. The
  add-on's compression adds to their saving, it does not replace it.
- **Their own peripheral compression** (`[DlssNr] SpatialCompression=true`, v0.8.9 and later, off
  by default): the model already runs at their work size (1728×972 on the stand); the add-on then
  warps that packed frame **a second time** (MAD 2.9 / 2.2 against their compression alone). The
  add-on cannot tell a packed frame from a normal one. Rule for users: use **one** compression —
  either theirs (`SpatialCompression`) or the add-on's (`Mode`), not both. Goes to INSTALL,
  TROUBLESHOOTING and LIMITATIONS.

```powershell
python tools/bench12/check_plan8_public_optiscaler.py --optiscaler "$env:TEMP\ofps-plan8-wilsjo2\v0.8.3" --output-root "$env:TEMP\ofps-plan8-t1" --candidate wilsjo2_v083
python tools/bench12/check_plan8_public_optiscaler.py --optiscaler "$env:TEMP\ofps-plan8-wilsjo2\v0.8.91" --output-root "$env:TEMP\ofps-plan8-t1" --candidate wilsjo2_v0891
```

## OptiScaler's own frame generation with the add-on (for Fallen Order)

Public OptiScaler has its own FG, also for D3D11 games through the bridge (upstream commit "Added
Dx11 FG Support Only Upscaler inputs"): `[FrameGen] Enabled=true`, `FGInput=upscaler`,
`FGOutput=dlssg` (Streamline 2.13 `sl.*.dll` and `nvngx_dlssg.dll` 310.8 in `OptiScaler\streamline`,
taken from the local private set). Probe on the D3D11 stand (pw_bench, DLAA, 3840×2160,
`--fps-cap 40`) with the Dagherbou build and the add-on (`Mode=2`, pixel path):

- DLSS-G runs: `DLSSG_Dx12::Dispatch Result: Ok` on every frame, Depth and Velocity tagged from the
  bridge's cached upscaler inputs.
- The add-on works at the same time: model adopted, re-created at 3456×1944, frames warped.
- Both runs finished, `device ok`, no errors.
- First attempt was not measurable (about 0.4 s per frame with and without the add-on): DaVinci
  Resolve held about 10.5 GB of the 16 GB VRAM.
- **Speed on a free GPU** (2026-09-25, wilsjo2 v0.8.3 with Fallen Order's settings: `Dx11Upscaler=ffx_12`,
  add-on Mode 2 80/90, temporal mode 1 with N=4; 600 frames, DLAA 3840×2160, uncapped; game frames,
  before DLSS-G adds its frames):

  | | without the add-on | with the add-on |
  |---|---|---|
  | FG off | 24.25 ms (41.2 fps), σ 5 % | 16.42 ms (60.9 fps), σ 36 % |
  | FG on | 27.70 ms (36.1 fps), σ 5 % | 19.82 ms (50.4 fps), σ 29 % |

  DLSS-G dispatches on every frame (590 × `Dispatch Result: Ok`), the add-on warps from the first
  frames, `device ok` in all four runs. FG costs about 3.4 ms of game frame time either way. The
  uneven frame time with the add-on is the synchronous cadence (a full model pass every 4th frame):
  under OptiScaler the background mode runs as the synchronous one. The number of frames shown by
  DLSS-G is not counted by the stand.

So Fallen Order can get frame generation from OptiScaler itself, with the add-on doing NR
compression; our own presenter is not needed. Owner checks in the game: generated frames visible,
HUD without glitches (upscaler input may need OptiScaler's Hudfix), frame rate.

## Not covered here

Games (007 First Light, Fallen Order): the owner checks in the game during rollout. HDR above 1,
switching HDR at run time, and the game's own frame generation together with NR are game checks.

## Command

```powershell
python tools/bench12/check_plan8_public_optiscaler.py --optiscaler <OptiScaler checkout>\x64\Release\a\OptiScaler.dll --output-root $env:TEMP\ofps-plan8-t1 --candidate d3d12_r1
```
