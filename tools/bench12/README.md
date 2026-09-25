# pw_bench12

Temporal add-on validation uses a separate `run_addon/` runtime. Run `powershell -File deploy_addon.ps1`
after building the SDK to copy the locally installed ReShade 6.8 add-on build, renodx consumer and
forwarder, plus the local bench/DLSS files from `run_fork/`. `-GameSource`, `-Build` and `-Runtime`
override source/destination directories. The script never downloads files or writes into a game.
An existing `ReShade.ini` is preserved. No game executable is launched.

`python temporal_quality.py LABEL` runs follow and forward (`--move-speed 7`), reference mode 0 and
N=4/N=8, three runs each. `--mode 3` tests background mode. It preserves the original ini, saves
per-run settings/logs, captures frames 144..167 of 168, and records exact commands and binary hashes.
Run `python temporal_metrics.py run_addon/LABEL` for RGB8 MAD and temporal-difference MAD versus
the corresponding mode-0 run, with medians/ranges. Metrics need the locally installed NumPy/Pillow.
`--compare-build run_addon/BASELINE` compares matching captures across builds instead.
These are tone-mapped 1080p measurements, not HDR-buffer or 4K error bounds.
For the D3D11 smoke harness `../bench/run_modes.sh`, the runtime additionally needs the locally
installed `dlss5-dx11-bridge.addon64`. Verify feature 18 creation/adoption and temporal activity in
ReShade.log; a successful D3D11 bench exit without the bridge does not exercise temporal NR.
The smoke script preserves `bench_stdout.log` and returns the executable's exit code.
See [the shared pass contract](../../core/shaders/temporal_passes.md) and
[feature parity](../../docs/dev/temporal-parity.md).

## Current shared-core validation

Active `run_addon/`, `run_r521/` and `run_nrhost/` ReShade.ini files use
`[OptimizerFPS]`; the reference captures retain their original legacy section.
`run_auto/` has archived captures but no active root ReShade.ini. `run_fork/`
and `run_fork_core/` use `OptiScaler.ini` section `[OptimizerFps]`, which is
separate from ReShade's section. A direct `--host core` run uses CLI settings
and no ReShade.ini. `deploy_addon.ps1` and `deploy_nrhost.ps1` stage the x64
add-on, core DLL and 21 DXBC; `deploy_fork.cmd` preserves fork files from
`PW_FORK_SOURCE` while `stage_fork_core.ps1` stages the core/DXBC from this
repository's x64 build.

From the repository root, use the same candidate name in every runtime:

```powershell
powershell -NoProfile -File tools/bench12/reference_dumps.ps1 -Runtime run_addon -Out candidate_p4t7
powershell -NoProfile -File tools/bench12/reference_dumps.ps1 -Runtime run_r521 -Out candidate_p4t7
powershell -NoProfile -File tools/bench12/reference_dumps.ps1 -Runtime run_nrhost -Out candidate_p4t7
powershell -NoProfile -File tools/bench12/reference_dumps.ps1 -Runtime run_nrhost -Case core_warp,core_off,core_warp_t1 -Out candidate_p4t7/core
python tools/bench12/compare_plan2.py --candidate candidate_p4t7
```

The comparison checks every case, manifest, frame 120/239 and NR audit. It
requires identical T0 output and limits T1 RGB MAD to 0.05. The bench9 x86
runtime is staged by `tools/bench9/deploy.ps1`: the x86 remote add-on comes
from `out/build/x86-remote`, while its `host64/` receives the x64 add-on,
core and DXBC from `out/build/x64`.

## Static-core regression (2026.9.1, plan 1)

The archived plan-1 `reference_dumps.ps1` run used the x64 add-on (with static `ofps_core`) and exactly 21
DXBC files, then records commands, settings, binary/source hashes, logs and frames
120/239 in a new output folder. Keep `reference_before_2026.9.1` unchanged.

The old absolute repository path in archived `manifest.json` files records where the
reference was captured. Comparisons read the files in the current reference folder;
the old path does not need to exist.

From the repository root:

```powershell
powershell -NoProfile -File tools/bench12/reference_dumps.ps1 -Runtime run_nrhost -Out candidate_task10
powershell -NoProfile -File tools/bench12/reference_dumps.ps1 -Runtime run_r521 -Out candidate_task10b
python tools/bench/compare_reference.py tools/bench12/run_nrhost/reference_before_2026.9.1 tools/bench12/run_nrhost/candidate_task10 --pattern "*_dump_*.bmp"
python tools/bench/compare_reference.py tools/bench12/run_nrhost/reference_before_2026.9.1 tools/bench12/run_nrhost/candidate_task10 --pattern "*_t1_dump_*.bmp" --mad-limit 0.05
```

Repeat the folder comparisons for `run_r521` and `run_addon`. The strict all-dump
comparison reports byte identity; a temporal-only comparison may use RGB MAD 0.05.
`run_addon` has 7 cases, `run_r521` has 9 (both use RR), and `run_nrhost` has 13
(SR + native NR). An existing nonempty output is rejected; choose a new folder for
another run. Match the saved manifest/INI and compare complete add-on log messages,
normalizing addresses and timings only. A repeated `host resources` diagnostic block
in the reference and the accepted `no grid` counter change are documented exceptions.
Require `device ok`, the warped marker outside Mode Off, and temporal-ready in T1.

Bench9 `run_matrix.ps1 -Only notemp,plain,game -CapturesRoot <absolute-path>` is a
separate smoke check of `first warped evaluate completed` and `temporal machine ready`
where applicable, not a dump-parity test. D3D11 NR remains SKIP without its bridge.

Local plan-1 acceptance on 2026-09-22:

| Runtime | Candidate | Cases | Byte-identical dumps |
| --- | --- | ---: | ---: |
| `run_addon` | `candidate_task10` (retained prior run, comparison repeated) | 7 | 14/14 |
| `run_nrhost` | `candidate_task10` | 13 | 26/26 |
| `run_r521` | `candidate_task10b` | 9 | 18/18 |

All manifests report successful cases and `device ok`; commands/modes match the
references. The strict comparison passes even for temporal dumps; the separate
temporal comparison with `--mad-limit 0.05` also passes. Complete add-on messages in
the four `*_t1_ReShade.log` files match after address normalization and the accepted
diagnostic exceptions: a duplicate host-input block in the reference and `no grid`
117 to 120. The restored `the other NR feature is gone; ...` message is present in
`run_r521/candidate_task10b/native_warp_ReShade.log`.

Bench9 `run/candidate_task10` completed all three smoke cases with exit 0: `game`
logged both first-warped and temporal-ready, `notemp` logged first-warped only, and
`plain` logged neither (Mode Off, T0). No bench9 dump comparison was performed.
D3D11 NR was skipped because the bridge is absent.

Final checks: all four Release presets built with `/WX`; x64 ctest passed 18/18 and
x86 10/10 without skips; `check-core-includes.py` and PowerShell 5.1 lint passed.
The release ZIP was produced in ignored `dist-release/`; installer tests reported
70 passed, 0 failed and 2 skipped (manual adoption and pre-26.26 files require the
missing `peripheral-warp.addon64.pre2625`). Existing size exceptions remain unchanged:
`core/shaders/temporal.hlsl` 1145 lines, `tests/test_d3d12.cpp` 739 and
`tests/test_d3d11.cpp` 586. No D3D11 or bench9 byte-parity claim follows from this smoke.

A standalone D3D12 / DXR 1.1 path-traced benchmark for the glTF lab scene. It produces noisy linear HDR colour and guide buffers, with optional DLSS Super Resolution (SR) or Ray Reconstruction (RR). The default remains `--upscaler none`.

Skinned meshes, animated morph weights, and meshes below animated TRS nodes update each frame. Scripted animation and camera time advance at a deterministic 60 frames per simulated second; interactive animation uses wall time clamped to 1/15 second. The default is one path per render pixel with four indirect bounces. Every surface uses a diffuse/GGX mixture with material roughness and Fresnel-based lobe selection. Metallic-roughness and tangent-space normal textures are supported. Each path vertex samples the sun and uses eight RIS lamp candidates with one lamp shadow ray; secondary lamp emission is combined using complementary MIS weights.

Camera-ray single-scattering haze is enabled by default. Presentation adds threshold-free bloom and ACES-like tone mapping after optional NGX reconstruction. Auto exposure is opt-in and presentation-only. The default upscaler remains `none`.

## Interactive scene lab (milestone 4)

Run `run_lab.bat` beside the executable, or:

```bat
pw_bench12.exe --interactive --gltf ..\bench\assets\lab_scene.glb --upscaler rr
```

### With the OptiScaler fork in the chain (path tracing -> RR -> Neural Rendering)

`run_lab_fork.bat` runs the same lab from `run_fork\`, where `deploy_fork.cmd` puts the bench next to the
fork's `dxgi.dll`, `nvngx.dll_dlssnr.dll`, `nvngx_dlssnr.dll`, `optimizer-fps-dlss5\` and `OptiScaler.ini`.
The fork's files are copied once from `PW_FORK_SOURCE` (default: the 007 First Light `Retail` folder, where
the fork is installed); an ini tuned in `run_fork\` is never overwritten, and a deleted file is copied again.
The bench calls NGX the ordinary way, so the fork answers those calls exactly as it does in a game: RR is
created by the "game", Neural Rendering is inserted after it. `run_fork\OptiScaler.log` says so
(`DLSS-NR running at ...`), and the overlay's upscaler time then covers RR plus the model. `Insert` opens the
fork's menu (compression, model resolution, temporal carry), `F1` the bench's own. `run_fork\` is not tracked.

Interactive mode runs until the window closes, permits resizing/maximizing, and defaults to vsync.
F1 toggles the menu; F2 toggles the independent timing overlay. Hold RMB to look (cursor hidden,
confined, and recentered); WASD move, Q/E move down/up, Shift multiplies speed by four, Ctrl by 0.25,
and the wheel changes base speed. Home restores the static view behind the character. Ctrl+F5..F8
store four camera bookmarks; F5..F8 recall them. Input respects ImGui capture and window focus.
Smooth motion preserves previous-frame matrices for motion vectors while restarting accumulation.

The Render menu controls None/SR/RR, Performance/Balanced/Quality/DLAA, paths per pixel, bounces,
lamp candidates, accumulation, sun strength/travel direction/angular diameter, haze, exposure,
auto exposure, bloom, tone mapping, firefly clamp, FOV, vsync, and all nine views. Upscaler creation
failure is shown in the menu and falls back to None. Resize and quality/upscaler changes wait for
the queue, recreate guides/output and NGX, and restart history. Shader/display descriptor slots
are reused so repeated changes do not exhaust the heap.

Accumulation freezes at N frames (2..4096), or continues with Infinite, and restarts when camera
or render settings change. With accumulation enabled, Final displays the running average.
“Feed the upscaler the accumulated image” defaults off: DLSS still evaluates the independent noisy
input, while the average is displayed directly. When enabled, DLSS receives an FP16 copy of the
FP32 running average and Final displays its reconstructed output. Noisy input always shows the
independent current sample; Accumulated always shows the reference. Depth, Motion vectors, Normals,
Roughness, Diffuse albedo and Specular albedo bypass display effects.

Lamps are grouped by glTF material name, removing a trailing dot plus exactly three digits.
Each group shows triangle count and integrated power, intensity (0..20), tint, Solo, and Reset;
Reset all lamps restores every group. Base emissions stay unchanged in triangle materials.
A small GPU table scales visible emission, surface/haze light sampling, and BSDF-hit MIS consistently.
While dragging, the CPU power CDF and group table are rebuilt/uploaded together at most once per
100 ms. Disabled groups are excluded from sampling, including the all-off case. Click a visible
emissive surface to select/highlight its group and scroll the list. Picking is entirely GPU based:
one requested render pixel writes its primary hit's triangle/group IDs (or 0xFFFFFFFF on a miss)
to an 8-byte UAV, copied to readback and consumed next frame after the submission fence.
No CPU ray cast or optional flashing outline is used.

Interactive startup loads `settings.json` beside the executable if present. Save settings and
normal window close atomically replace that file. `--settings file` loads an explicit path in
either mode; relative paths are relative to the working directory. Explicit CLI options win at
startup and on Reload settings. Saved state includes every menu control, camera pose, bookmarks,
movement speed, menu/overlay visibility, and lamp factors keyed by canonical name. A saved camera
is used by scripted runs unless camera-related CLI switches explicitly override it.

```bat
pw_bench12.exe 120 --settings settings.json --gltf ..\bench\assets\lab_scene.glb --dump 110
```

The version-1 JSON uses an `options` object with CLI-style names and string values, an
`accumulation` object, `camera`/four `bookmarks` (null or eye/target vectors), and a `lamps`
array of name/intensity/tint records. The handwritten parser rejects malformed JSON, duplicate
keys, invalid ranges and camera poses; loading is transactional. Save writes a sibling .tmp file
then replaces the destination. Reload/Save errors stay visible in the menu. The launcher supplies
the requested good defaults explicitly, so those switches also override saved values on launch;
launch the exe directly to restore all tuned values.

Dear ImGui **v1.92.5-docking** comes from
[ocornut/imgui](https://github.com/ocornut/imgui/tree/v1.92.5-docking), unpacked into
`external/imgui/`. Preserve its bundled MIT license. `external/` is ignored; this is a local
build dependency, like the supplied NGX headers. The build compiles only imgui.cpp, imgui_draw.cpp,
imgui_tables.cpp, imgui_widgets.cpp and the Win32/D3D12 backends with /W0. Bench sources retain
/W4 /WX and DXC retains warnings-as-errors.

New modules: `application.*` owns the scripted/interactive loops; `flight.*` handles navigation;
`menu.*` and `menu_render.cpp` integrate ImGui and controls; `settings.*` persists validated
state; `json.*` implements JSON syntax; `lamps.*` owns CPU grouping/CDF math;
`scene_lamps.cpp` uploads group factors; `camera_angles.h` converts sun/navigation angles;
`device_resize.cpp` handles swap-chain resizing; `pathtrace_resources.cpp` handles render-target,
NGX and pick-readback lifetimes; `tests_interactive.cpp` covers these CPU contracts.

NGX shutdown still fences the GPU, releases the feature, destroys its single owned parameter map,
and calls Shutdown1(device) before releasing renderer/device resources. Initialization path strings,
search-path pointers and the feature-info block now live in the NGX owner rather than temporary
stack storage. The bare-init retry remains. The driver core is pinned for process lifetime to
prevent late worker/TLS callbacks into an unloaded DLL. Re-created features reset the output-state
tracking flag. These address lifetime hazards found by inspection; the reported RR exit access
violation cannot be declared fixed until reproduced and retested on the target GPU.

Verification: build.cmd runs the CPU suite, including settings memory/disk round trips and atomic
replacement, explicit CLI precedence, malformed JSON rejection, .NNN grouping, tinted/disabled/all-off
CDF rebuilds, and azimuth/elevation conversions. GPU verification remains required for interactive
controls/capture, primary-hit picking (including alpha masks and resized output), nine views,
N/infinite accumulation and resets, accumulated DLSS input, live lamp changes, minimize/resize,
repeated None/SR/RR and quality switching, error fallback, frame dumps, timing, RR shutdown and
clean D3D12 debug-layer output. CPU tests do not initialize D3D12 or NGX.

## Requirements

- Windows 10/11 x64 with an up-to-date graphics driver.
- Hardware feature level 12_1, DXR tier 1.1, shader model 6.5, and typed UAV support for the output formats. Unsupported devices fail explicitly.
- Visual Studio 2022 C++ x64 tools (C++20).
- Windows SDK **10.0.26100.0**, including `C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\dxc.exe`.
- Windows Graphics Tools optional feature for `--debug-layer`.

Only Windows SDK libraries are linked. NGX headers are supplied in `external/ngx`; no NGX import library, CUDA SDK, or CMake is needed. Interactive UI uses the locally supplied Dear ImGui sources. The shared loader is included directly from `../bench/pw_gltf.h`.

For SR/RR, use an NVIDIA RTX GPU and a driver supporting the selected feature. Place `nvngx_dlss.dll` (SR) or `nvngx_dlssd.dll` (RR) from a compatible NVIDIA DLSS SDK beside `pw_bench12.exe`. Missing feature DLLs fail with their exact expected path; the bench does not download them. `none` needs neither DLL nor NGX. `--nr` needs `nvngx_dlssnr.dll` (310.8 was used) and the built `nvngx.dll_pwbench12.dll` beside the exe; it does not go through the driver core. The driver core `_nvngx.dll` is loaded from the newest matching `C:\Windows\System32\DriverStore\FileRepository\nv_dispi.inf_amd64_*` directory containing it. NGX entry points are resolved with `GetProcAddress`; incompatible drivers fail explicitly.

## Build and checks

From a command prompt:

```bat
cd /d <repository>\tools\bench12
build.cmd
obj\bench12_tests.exe ..\bench\assets\lab_scene.glb
```

From another directory, use the full path:

```bat
cmd /c tools\bench12\build.cmd
```

The script locates Visual Studio, calls `vcvars64.bat`, compiles the compute shader as `cs_6_5`, compiles the presentation shaders, builds `pw_bench12.exe`, the NR call forwarder `nvngx.dll_pwbench12.dll` and the CPU tests, and runs those tests. From PowerShell use `& .\build.cmd`; from Git Bash call it through PowerShell rather than `cmd /c`. Both DXC and bench C++ warnings are errors (`/W4 /WX` for C++); only third-party ImGui implementation files use /W0. Two existing shadowing warnings are suppressed only around the shared loader include. Build outputs remain inside this directory and are ignored by Git.

The CPU tests cover invalid options and milestone defaults, the `--nr`/`--mv-format` options, the feature-18 parameter writes and the host parameter block's type conversions, camera phases and dolly limits, accumulation/camera-cut detection, projection depth and motion conventions, NGX quality selection, RR creation/evaluation contracts, row-vector matrix conversion, RIS normalization/integration with zero-target and occluded candidates, specular-albedo roughness/view dependence, flat/RLE HDR decoding, truncated HDR rejection, and optional loading/deformation/image decoding of the actual GLB. Reservoir math and the specular guide function are shared directly with HLSL. Tests do not create a GPU device or load NGX.

## Run

Run from `tools/bench12` so the relative asset paths below resolve:

```bat
pw_bench12.exe 40 --gltf ..\bench\assets\lab_scene.glb --camera static --cam-dolly 2.9 --cam-lift 0.35 --fov 62 --sun-dir -0.30,-0.80,0.52 --sun-strength 400 --albedo 0.4 --dump 30
```

Successful completion prints `[info] bench finished after 40 frames, device ok` and writes `dump_30.bmp` beside the executable. Dumps are zero-based and capture the full-size, tone-mapped/debug image copied from the back buffer before presentation. Shader binaries are always loaded from `shaders/bin` beside the executable; input paths are relative to the working directory.

Append `--debug-layer` for D3D12 validation. All stored D3D12 messages are printed at initialization and after submissions; error/corruption messages fail the run. Check every view on a DXR device, confirm cut-out shadows, and compare static `--view accum` frames with individual noisy frames. A moving camera must reset accumulation. No GPU run or clean debug-layer result is claimed by the CPU tests.

To compare upscalers, append `--upscaler sr` or `--upscaler rr` to the command above. SR receives the noisy image as a baseline; it is not a path-tracing denoiser. Colour presentation and dumps use the full-resolution upscaler output. `--view albedo|normal|depth|motion|accum` continues to show the original guides/reference. Dump output remains tone-mapped BMP; the optional `--hdr-out` extension is not implemented.

Each nonempty run prints `[info] gpu frame time: avg X ms (path trace Y ms)` using direct-queue timestamp queries. Frame time includes tracing, NGX, barriers, and the tone-map draw, but excludes initialization, dump copies, CPU work, and the Present/vsync wait. Submissions remain serialized. `--vsync 0` uses tearing when supported and unsynchronized Present otherwise; `--vsync 1` uses interval one.

## DLSS Neural Rendering host (`--nr`)

The bench can be the application that creates and evaluates DLSS Neural Rendering (NGX feature 18), so the
Optimizer FPS add-on's feature-18 hook can be tested without a game and without renodx-dlss5.

- `--nr native` runs feature 18 after the chosen upscaler, on the full-resolution colour (the SR/RR output, or
  the render colour with `--upscaler none`): feature, colour and output at that size, depth and motion at render
  size with their own sub-rects, output in a separate RGBA16F texture that is presented.
- `--nr upscale` (no DLSS) creates the feature at the render size and evaluates with colour/guides at render
  size and the output at output size. The 310.8 runtime refuses this with `0xBAD00005`; the bench reports it and
  presents the render colour. [NGX_PARAMETERS.md](NGX_PARAMETERS.md) lists every combination that was tried.
- The NGX core refuses the available `nvngx_dlssnr.dll` (its signature does not verify), so the bench loads it
  directly and calls it through `nvngx.dll_pwbench12.dll` (built by `build.cmd`); both must be beside the exe.
- The runtime is loaded on the first frame and feature 18 is created on the second, after one present: the
  add-on installs its hooks at present time and must see the create, as in a game.
- `[nr] frame N: mode ..., feature WxH, colour WxH, output WxH, result 0x...` is printed for the first three NR
  frames, every 60th frame and on a change of result; the first create and evaluate print every parameter as
  `[nr parameter]`; the runtime's own log (`nvngx_dlssnr_310_8_0.log` beside the exe) is echoed as `[nr log]`.

```bat
rem control run without ReShade, from tools\bench12 (needs nvngx_dlssnr.dll beside the build)
pw_bench12.exe 240 --gltf ..\bench\assets\lab_scene.glb --upscaler sr --nr native --sun-dir 0.45,-0.77,0.45 --sun-strength 1500 --exposure 0.22 --haze 0.008 --fov 62 --dump 120,239
pw_bench12.exe 240 --gltf ..\bench\assets\lab_scene.glb --nr upscale --sun-dir 0.45,-0.77,0.45 --sun-strength 1500 --exposure 0.22 --haze 0.008 --fov 62
```

`powershell -File deploy_nrhost.ps1` builds the `run_nrhost\` runtime from `run_addon\` without
`renodx-dlss5.addon64` (ReShade 6.8 as `dxgi.dll`, the Optimizer FPS add-on with its shaders and forwarder,
DLSS/NR 310.x DLLs) plus this build's exe, forwarder and shaders, and copies `nvngx_dlssnr.dll` into the build
directory for control runs. `-AddonBuild ..\..\out\build\x64` takes the add-on and its shaders from an SDK
build instead. An existing `run_nrhost\ReShade.ini` is kept. Run from `run_nrhost\` with
`..\..\bench\assets\lab_scene.glb` and read `ReShade.log` (lines with `Optimizer FPS`). The bench leaves
through `TerminateProcess` after NGX use, so the add-on never unloads: set `CrashGuard=0` in `[OptimizerFPS]`,
otherwise the next run starts in the add-on's safe mode. For a control run inside `run_nrhost\`, rename
`dxgi.dll` (for example to `dxgi.dll.off`); ReShade and the add-on are then not loaded. `run_nrhost\` is not
tracked.

## Options

| Option | Default | Meaning |
| --- | --- | --- |
| positional frame count | `600` | Number of frames; closing the window stops early. |
| `--gltf file.glb` | `../bench/assets/lab_scene.glb` | Scene loaded through the existing bench loader. |
| `--hdri file.hdr` | constant sky | Radiance RGBE, `-Y +X`, flat or scanline RLE. Invalid input fails explicitly. |
| `--width W --height H` | `1920 1080` | Window client area and RGBA8 flip-model swap-chain size; fixed in scripted mode, resizable in interactive mode. |
| `--render-scale S` | `0.5` | Render/guide dimensions: output times S, truncated, minimum one pixel. Range `(0,1]`. |
| `--camera MODE` | `script` | `static`, `yaw`, `forward`, `strafe`, `combo`, the repeating five-phase `script`, or animated-character `follow`. |
| `--yaw-speed D` | `45` | Degrees/second; camera path matches the D3D11 bench. |
| `--move-speed M` | `2` | Metres/second of the scripted `forward`, `strafe` and `combo` motion (the interactive fly speed is around 7). |
| `--sway M` | `0` | Idle eye sway amplitude in scene units. |
| `--cam-dolly M` | `0` | Move toward the target, retaining at least 30% of the original distance. |
| `--cam-lift M` | `0` | Raise eye and target together. |
| `--cam-pos x,y,z --cam-target x,y,z` | scripted | Supply both to override the scripted pose, in bench coordinates. Dolly, lift, and sway still apply. |
| `--fov D` | `60` | Vertical FOV in degrees. |
| `--sun-dir x,y,z` | `0,-1,0` | World direction the light **travels**; normalized and negated when sampling toward the sun. |
| `--sun-strength E` | `1` | White sun irradiance multiplier. |
| `--sun-angle D` | `0.5` | Full angular diameter in degrees; zero gives a directional light. |
| `--exposure E` | `1.45` | Linear radiance multiplier before firefly clamping and presentation. |
| `--albedo A` | `1` | Base-colour multiplier, excluding lamp emission; reflectance clamps to `[0,1]`. |
| `--firefly L` | `50` | Maximum sample luminance after exposure. This deliberate clamp biases extreme samples. |
| `--spp N` | `1` | Lighting samples per pixel, `1..4096`; primary jitter is shared within a frame. |
| `--bounces 0..8` | `4` | Indirect bounces per path; every vertex receives sun and lamp NEE. Secondary lamp emission is included with MIS. |
| `--light-candidates M` | `8` | RIS lamp candidates per vertex, `1..1024`; at most one selected lamp shadow ray. |
| `--vsync 0\|1` | `0` | Unsynchronized/tearing presentation or interval-one vsync. |
| `--upscaler none\|sr\|rr` | `none` | No upscaler, DLSS SR, or DLSS RR. Quality is derived from render scale. |
| `--nr off\|native\|upscale` | `off` | DLSS Neural Rendering host (NGX feature 18), see [below](#dlss-neural-rendering-host---nr). `upscale` requires `--upscaler none`. |
| `--nr-colour srgb\|linear` | `srgb` | Colour handed to NR: sRGB-encoded proxy (white 1.0, soft knee) decoded afterwards, or linear HDR as rendered. |
| `--nr-log 0\|1\|2` | `1` | NR runtime log level echoed as `[nr log]` (each distinct line once); 0 leaves its log off. |
| `--nr-output-pad X,Y[,BX,BY]` | none | NR output texture X/Y texels larger than the frame, region at BX,BY (default 0,0); the pad is filled with magenta before each evaluate and checked on report frames. Only the region is presented and dumped. |
| `--nr-colour-pad X,Y[,E]` | none | NR colour (the sRGB proxy) X/Y texels larger than the frame, region at 0,0: the shape of RenoDX DLSS by ShortFuse (a render region inside a full-size texture). The pad is black, or the edge repeated with E=1; a warped run that gives the same frame both ways reads nothing past the region. Needs `--nr-colour srgb`. |
| `--mv-format rg16f\|rgba16f` | `rg16f` | Motion texture handed to DLSS and NR: the RG16F guide, or an RGBA16F copy (xy motion, zw 0) as some games use. |
| `--jitter 0\|1` | `1` | Halton(2,3) projection jitter in render pixels, positive right/down. |
| `--depth standard\|reverse` | `standard` | Unjittered projection, near `0.1`, far `300`; miss is `1` standard or `0` reverse. |
| `--view VIEW` | `colour` | `colour`, `noisy`, `accum`, `depth`, `motion`, `normal`, `roughness`, `albedo`, `specular`. |
| `--dump N[,N...]` | none | Zero-based frames to save as `dump_N.bmp` beside the executable. |
| `--haze D` | `0.012` | Camera-medium extinction per metre, nonnegative; zero disables haze. Scattering albedo is 0.9. |
| `--haze-g G` | `0.6` | Henyey-Greenstein anisotropy, strictly between -1 and 1. |
| `--tonemap aces\|neutral` | `aces` | ACES-like filmic fit or the original Reinhard curve. |
| `--bloom S` | `0.06` | Nonnegative strength of three threshold-free blurred pyramid copies. Zero disables addition. |
| `--auto-exposure 0\|1` | `0` | Presentation-only log-average exposure adaptation, one simulated second time constant. |
| `--debug-layer` | off | Enable D3D12 debug layer and print its messages. |
| `--help` | | Print usage without creating a device. |

The scripted camera is offset to the first mesh node whose name contains `girl` or `haracter`, using its time-zero bounds, matching the D3D11 bench. An explicit camera position bypasses that anchor.

## Buffers and presentation

All guides use the render resolution. `colour` is RGBA16F; `depth` R32F; `motion` RG16F; `normalRoughness`, `diffuseAlbedo`, and `specularAlbedo` RGBA16F; `specHitDistance` R16F. Motion is current-to-previous in render pixels, from unjittered matrices and interpolated `prevPos` (equal to `pos` for static geometry and paused animation). Specular albedo uses the environment-BRDF approximation with per-channel F0 `lerp(0.04, baseColour, metallic)`, roughness, and view angle. Specular hit distance records the actual primary specular BSDF ray's first hit, or zero for diffuse samples, misses, and zero-bounce paths. For explicit `--spp > 1`, it uses sample zero instead of averaging incompatible paths. R16F distances saturate at 65504. Primary misses clear guides except far depth.

`accum` keeps a separate RGBA32F running average and resets whenever eye or target changes. Jitter does not reset it. Colour and accumulation views use bloom, optional display exposure, and ACES-like tone mapping followed by sRGB encoding. `--tonemap neutral` selects the former Reinhard curve. Normal/depth views show their stored values; motion maps zero to middle grey and a 16-pixel displacement to a channel endpoint. The reference includes the configured firefly clamp.

## Source layout

- `main.cpp`, `options.h`, `application.*`: entry wiring, strict option parsing, and application loops.
- `math.h`, `camera.h`: loader-compatible math, projection, jitter, and camera path.
- `device.h/.cpp`, `device_dump.cpp`: device/window, queue/fence, descriptors, uploads, readback, BMP output.
- `device_timing.cpp`: timestamp queries, readback, and per-run GPU timing averages.
- `image.h/.cpp`: WIC decoding and Radiance HDR loading.
- `scene.h/.cpp`: glTF conversion and static/dynamic partitioning, material/texture buffers, emissive power CDF, camera anchor.
- `accel.h/.cpp`: static/dynamic opaque/cut-out BLAS geometries, dynamic refits/rebuilds, and one/two-instance TLAS.
- `pathtrace.h/.cpp`, `pathtrace_pipeline.cpp`: output resources, constants, dispatch/present, root signatures, and PSOs.
- `ngx.h/.cpp`, `ngx_evaluate.cpp`: dynamic driver loading, NGX lifetime, helper bridges, parameter logging, and SR/RR evaluation barriers.
- `ngx_sdk.h`, `ngx_contract.h`: D3D-only SDK includes, quality/flag selection, matrix conversion, and testable RR parameter construction.
- `ngx_nr.h/.cpp`: the `--nr` host (feature creation after the first present, per-frame evaluate, barriers, `[nr]` report).
- `ngx_nr_runtime.h/.cpp`, `ngx_nr_forwarder.cpp`: direct loading of `nvngx_dlssnr.dll` and the `nvngx.dll_pwbench12.dll` call forwarder its caller check requires.
- `ngx_nr_contract.h`, `ngx_nr_params.h/.cpp`: testable feature-18 parameter writes and the host-owned NGX parameter block.
- `ngx_nr_bridge.h/.cpp`, `ngx_nr_pad.h/.cpp`, `ngx_nr_log.h/.cpp`: sRGB colour bridge and region copy, `--nr-output-pad` fill/check, runtime log echo.
- `motion_pack.h/.cpp`: `--mv-format rgba16f` copy of the motion guide (compute shader compiled at startup).
- `deploy_nrhost.ps1`: builds the `run_nrhost\` runtime.
- `shaders/pathtrace.hlsl`, `present.hlsl`, and `.hlsli` includes: ray queries, BRDF/light sampling, guides, accumulation, presentation.
- `shaders/reservoir.hlsli`, `specular_guide.hlsli`: RIS estimator math and the per-channel F0 implementation of [NVIDIA's EnvBRDFApprox2](https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideDLSS_RR.md#421-specular-albedo-generation), shared with CPU tests.
- `image_mips.h/.cpp`, `device_texture.cpp`: CPU colour/data/HDR mip chains and multi-subresource upload.
- `display.h/.cpp`, `shaders/display.hlsl`: linear bloom pyramid and exact weighted log-luminance reduction for display exposure.
- `pipeline.h/.cpp`: shared shader loading and root-signature creation.
- `shaders/bsdf.hlsli`, `haze.hlsli`, `sampling_math.hlsli`, `display_math.hlsli`: surface scattering, medium scattering, and CPU-tested sampling/display math.
- `tests.cpp`, `tests_materials.cpp`: CPU regression checks and shared-loader compile coverage, with optional actual-scene validation.

## NGX contract and review

The executable directory is both NGX's application data path and the sole extra feature search path in `FeatureCommonInfo.PathListInfo`. The plain driver-core initializer receives application ID `0x24480451` and the supplied SDK version. NGX logs are routed to stderr. Creation and the first evaluation print every helper parameter write as `[ngx parameter] name = value`, including null/zero optional fields and both matrices; subsequent frames print reset, jitter, dimensions, and simulation delta. See [NGX_PARAMETERS.md](NGX_PARAMETERS.md) for the exact RR parameter list.

Quality uses the nearest standard ratio: 1/3 Ultra Performance, 0.5 Performance, 0.58 Balanced, 2/3 Quality, and 1.0 DLAA. There is no separate `--quality` override. Actual requested render dimensions are passed unchanged. Nonstandard scales may be rejected by a particular feature DLL; this is reported as an NGX error rather than silently changing resolution.

RR uses DLUnified denoising, hardware (0..1) depth, and roughness packed in world-normal alpha. Flags are HDR and low-resolution motion, plus inverted depth only for `--depth reverse`. Motion scales are one, jitter is in render pixels, and matrices are unjittered, row-major, transposed for NGX's row-vector convention (see the [NVIDIA sample's matrix explanation](https://github.com/nvpro-samples/vk_denoise_dlssrr#matrices)). Exposure/pre-exposure are one because the input already includes the bench's exposure. Frame delta is 1000/60 ms in scripted runs and measured wall time clamped to 1000/15 ms in interactive runs. History resets at startup and on camera cuts (eye movement over one scene unit or view rotation over 45 degrees between frames); smooth camera motion retains NGX history while still resetting the separate accumulation reference.

All NGX inputs transition from UAV to `NON_PIXEL_SHADER_RESOURCE` before evaluation and back afterwards. Output is a distinct output-size RGBA16F UAV, transitioned to pixel SRV for presentation. The bench restores its descriptor heap and graphics root signature/bindings after NGX. Feature creation is submitted and fenced before the first frame; explicit feature release, parameter destruction, and device-specific shutdown happen while the device and queue are idle and alive, before guides or output are freed. Exception cleanup follows the same order. The driver core is pinned for process lifetime, including destruction of all Device-owned COM objects.

Runtime review is still required on an RTX GPU: DLL/driver compatibility, nonstandard quality ratios, packed roughness, matrix/jitter alignment, exposure treatment, and zero specular-hit distance for misses. The supplied RR header gives no matrix convention comment; the transpose follows NVIDIA's sample instead. No GPU execution, image quality, NGX success, timing value, or clean debug-layer run is claimed. Animated geometry, acceleration updates, and interactive resizing are supported; their GPU behavior still requires validation.

## Milestone 3 estimators and limits

Material roughness is the factor times the texture's green channel, clamped to `[0.04,1]`; metallic is the factor times blue, clamped to `[0,1]`. F0 is `lerp(0.04, baseColour, metallic)`, diffuse albedo is `baseColour*(1-metallic)`, and the specular sampling probability is luminance of view Fresnel clamped to `[0.1,0.9]`. Both sampling branches use the complete mixture PDF for throughput. Normal mapping derives tangent and handed bitangent from world triangle edges and UV differences; degenerate UVs and invalid/backfacing mapped normals fall back to the interpolated surface normal. The RR normal/roughness, diffuse, and specular guides use these same material values.

The shared loader appends fields with dielectric/no-texture defaults; old fields, material categories, and draw-range behavior remain intact. An absent metallic factor defaults to zero for legacy compatibility (rather than glTF's metallic default of one). Normal texture scale is honored. Only the existing `TEXCOORD_0` convention is supported; alternative UV sets and texture transforms are not added.

All uploaded material and environment textures have full CPU-generated box-filtered mip chains. Colour mips filter in linear light then encode to SRGB; metallic-roughness and normal copies are UNORM and filter without gamma conversion. An image used as both colour and data gets separate chains. Data copies are created only when used. Material LOD is `max(0, log2(max(distanceFromCamera*tanHalfFov*540/renderHeight,1)))`, consistently applied to visible surfaces, sampled lamps, and alpha tests. Environment LOD uses the environment/render-width ratio. This is a fixed distance heuristic, not a UV-density-aware ray cone.

For lamp MIS, let `qA = lampPower/(totalPower*area)`, `qL = qA*distance²/abs(lampNormal·-direction)` and `qB` be the diffuse/GGX mixture PDF. The deterministic weights are `wL=qL/(qL+qB)` and `wB=1-wL`. RIS selects using the unweighted unoccluded target, then multiplies its selected contribution by `wL` and `(sum(target/qA)/M)/selectedTarget`. The BSDF hit contributes emission times `wB`. No random reservoir normalization is treated as a PDF: these two estimators integrate complementary parts of the same function for every candidate count. The last vertex uses `wL=1`, since it cannot continue a BSDF path. Near-mirror lamp hits have a dominant BSDF PDF and retain essentially full emission instead of being discarded. Firefly clamping and path truncation still introduce bias.

Haze uses one truncated-exponential camera-distance sample per pixel, even with `--spp > 1`. Its weight is `0.9*(1-exp(-density*distance))`; it samples the sun and one RIS-selected lamp, with at most one shadow ray each. Lamp illumination includes medium transmittance to the lamp. The camera medium extends to the hit or 300 metres; the sun enters through a camera-centred 300-metre boundary. Surface radiance is attenuated, but depth/motion and other guides describe the surface behind the haze. Indirect segments do not scatter in the medium.

Display builds a linear pyramid after NGX (or from accumulation in that view). Three blurred copies at approximately 1/4, 1/16, and 1/64 resolution are averaged, scaled by `--bloom`, and added before tone mapping. The exact pixel-weighted average of log luminance is reduced to 1x1, including odd-sized edge tiles. Auto exposure targets `0.18/exp(mean(log(max(luminance,1e-5))))`, smoothed with `1-exp(-1/60)` per simulated frame starting at scale one. Debug views bypass these effects. `--exposure` still scales noisy HDR before NGX; display exposure never changes NGX flags or exposure parameters.

### Remaining SR initialization uncertainty

In the supplied source, SR and RR already called the same plain initializer with identical arguments. The only mode-dependent operation before it was checking whether the selected DLL existed; there was no SR preload or different device setup to remove. Mode selection and that check now happen after the common initializer, and its app ID, SDK version, and directory are logged. The supplied headers identify `0xBAD0000C` as `FAIL_OutOfDate`; the new diagnostic says so explicitly. This does **not** establish that SR initialization is fixed. Driver/feature compatibility and the reported SR-only behavior require reproduction on the target GPU. Both local feature DLLs report version `310.9.1.0`; they were not modified.

### Verification

`build.cmd` compiles all five shader entry points with DXC warnings as errors, the C++20 executable with `/W4 /WX`, and all CPU test translation units. Tests include a synthetic GLB covering new material parsing and unchanged legacy vertex fields/ranges, linear/SRGB/odd-size HDR mip filtering, RIS+BSDF MIS integration at 1/8/32 candidates, truncated medium sampling, HG phase normalization, metal guides, and exposure adaptation. `pw_gltf.h` is compiled through these tests; the full D3D11 bench is not built because its NGX SDK dependency is unavailable.

GPU verification remains required for image quality, normal-map handedness in real assets, bloom/haze appearance, exposure stability, SR/RR initialization and evaluation, teardown without an access violation, and a clean `--debug-layer` run. Resource-state transitions were reviewed statically: guide UAV -> compute SRV -> UAV -> pixel SRV; NGX output starts in UAV and returns from pixel SRV on subsequent evaluations; display source temporarily adds compute-read access and restores pixel-read state. Descriptor heaps and graphics bindings are restored after NGX.

## Animated characters (milestone 5)

```bat
pw_bench12.exe 120 --gltf animated_lab.glb --camera follow --follow-node hero --anim-start 0 --anim-speed 1 --dump 110
```

`--anim-speed S` defaults to 1 (zero freezes, negative values reverse playback).
`--anim-start T` defaults to 0; time wraps over the longest imported channel, including previous
positions across the loop seam. The first frame evaluates the requested start with zero object
motion; subsequent scripted frames advance exactly 1/60 second times speed. Interactive delta
uses real time, clamped to 1/15 second before applying speed. Pausing freezes animation time and
sets previous positions equal to current positions. SR/RR receive the frame delta in milliseconds,
independent of animation speed and pause.

The Animation menu provides Play, speed, time scrubbing, Step one frame, Follow character, dynamic
triangle count, CPU skinning milliseconds, and GPU BLAS refit/rebuild milliseconds from timestamp
queries (the latter excludes TLAS construction). Step pauses and advances one 1/60-second tick at
the selected speed. Scrubbing resets temporal history and accumulation. Animation motion resets
accumulation; a paused scene and stationary camera can converge. `--anim-playing 0` starts paused.
Playback state, speed, scrub position, follow mode/node, and rebuild interval persist in settings.

`--camera follow` follows the animated world position and horizontal local -Z heading of
`--follow-node NAME`, or the first node whose name contains `hero` or `girl`. It uses the static
third-person distance/height, including dolly/lift, with exponential smoothing. Enabling follow
without a matching node reports a clear error at startup; the menu toggle is disabled when none
exists. Manual flight or a camera bookmark takes control and disables follow; turning it off
leaves the camera at its current pose. Smoothing settles to the exact target to allow accumulation.

At load, triangles are stably grouped as static opaque, static masked, dynamic opaque, dynamic
masked. A node is dynamic when skinned, targeted by a weights channel, or below any animated TRS
node (including itself). Materials and vertices remain single GPU buffers. The dynamic range
follows the static range; InstanceID selects its base and GeometryIndex selects its opaque/masked
offset for both candidate alpha tests and committed hits. Dynamic emissive triangles retain visible
emission, but have no lamp group/CDF entry or lamp MIS proposal.

The static BLAS is built once. The dynamic BLAS uses ALLOW_UPDATE | PREFER_FAST_BUILD,
refits when the evaluated time changes, and rebuilds every `--blas-rebuild N` updates
(default 60, minimum 1). The tiny TLAS rebuilds only after a dynamic BLAS update.
The first paused frame uploads prevPos = pos without refitting unchanged geometry;
subsequent identical time/previous-time pairs skip evaluation, upload, BLAS and TLAS work.
An all-dynamic scene has only the dynamic instance. An empty dynamic set allocates no
dynamic BLAS/upload/workers and retains the original static TLAS.

CPU evaluation balances persistent workers by unique vertex count, splitting large
primitives into ranges. A condition-variable generation/completion barrier publishes
one read-only pose to all workers. WorldsAt runs once per new pose; world joint palettes
are computed once per skin and converted once per mesh-node basis (shared skins can be
instanced under different transforms). Morph weights are sampled once per dynamic node.
Workers blend up to four affine 3x4 matrices before transforming each position and normal.
All frame storage is reserved at load; there are no per-frame thread creations or getenv calls.

Consecutive playback uses cached unique-vertex positions for prevPos. A seek evaluates
an additional previous pose only when it differs from both the cached pose and current time.
A precomputed per-primitive source-index/output mapping scatters directly into the mapped
TriVertex buffer in its final opaque/masked triangle layout. UVs and materials are written
once at load. No fat pwgltf::Vertex intermediate or serial per-frame packing pass remains.
The mapped upload buffer is never read for history. Only the dynamic range is copied into
DEFAULT memory so repeated shader/BLAS reads stay in GPU memory. Synchronous submission
fences protect upload reuse; asynchronous rendering would require fenced upload slots.

The overlay shows wall-clock frame milliseconds beside GPU milliseconds. At exit,
`[info] cpu animation: avg X ms, blas refit Y ms` reports mean CPU evaluation/output time
and mean GPU BLAS refit/rebuild time over rendered frames (skipped frames contribute zero;
BLAS timing excludes TLAS construction).

New modules: `animation.h/.cpp` owns CPU workers and imported animation data; `animation_time.h`
owns looping/clock rules; `scene_animation.cpp` handles vertex upload and node poses;
`follow_camera.h` owns follow smoothing; `menu_animation.cpp` owns controls; `tests_animation.cpp`
provides the synthetic GLB and regression tests. The shared `../bench/pw_gltf.h` adds
`Primitives`, `DynamicNode`, `VertexWorkspace`, and `BuildVerticesSubset`; the legacy
`BuildVertices` and `WorldsAt` signatures and vertex layout remain supported.

Verification: `build.cmd` builds the C++20 application and CPU tests with /W4 /WX, compiles all
five shader entry points with DXC -WX, and runs the CPU suite. Tests cover a two-key skinned quad
plus a static mesh loaded from a synthetic GLB, animated ancestors/morph classification, full vs
subset positions/normals, previous positions, pause, looping, threaded vs serial evaluation,
clock clamping/stepping, follow smoothing, options, and settings round trips. The optional real
lab CPU check reports its dynamic primitive count and verifies static subset positions at a later
time when that set is empty. The D3D11 bench also builds with the supplied headers by setting
`PW_NGX_SDK_ROOT` to `tools/bench12/external/ngx` before running `tools/bench/build.cmd`.

### CPU animation performance checks

The default CPU suite automatically loads `../bench/assets/lab_scene.glb` relative to the
bench directory when present, or prints an explicit skip when absent. A positional path
overrides that asset and also runs the existing full-scene/image decoding checks:

```bat
cmd /c tools\bench12\build.cmd
tools\bench12\obj\bench12_tests.exe tools\bench\assets\lab_scene.glb
```

`tests_animation_perf.cpp` measures 60 consecutive frames at 60 Hz after initialization,
then 60 paused frames. `tests_animation_baseline.h` preserves the original persistent-worker,
primitive-partitioned pipeline including its large intermediate buffers and serial packing.
The optimized benchmark includes direct output writes. A second benchmark uses
`VirtualAlloc(PAGE_WRITECOMBINE)` to approximate an upload heap's CPU memory policy without
creating a GPU device. It separately reports the one-time pause transition. Timings exclude
asset loading, allocation, reference comparisons and actual GPU copies/refits.

On the available lab (465,931 dynamic triangles, 262,085 unique vertices, 34 primitives,
32 logical CPUs), the sequential implementation checkpoints were:

| Checkpoint | Playing ms/frame | Paused ms/frame |
| --- | ---: | ---: |
| Original evaluation, excluding serial upload packing | 12.7348 | 12.680027 |
| 1. Skip unchanged time pairs | 11.9283 | 0.000005 |
| 2. Reuse previous deformation | 11.2227 | 0.000003 |
| 3. Direct TriVertex output (packing now included) | 4.6808 | 0.000003 |
| 4. Balance by unique vertex ranges | 4.1507 | 0.000002 |
| 5. Share world transforms and skin palettes | 2.9291 | 0.000003 |
| 6. Blend affine matrices before transforming | 2.8517 | 0.000003 |

The final strict-build run measured **20.3340 ms/frame legacy versus 2.6317 ms/frame
optimized** in cached memory (legacy paused: 20.6706 ms/frame). Write-combined output
measured **4.5113 ms/frame playing**, **0.000007 ms/frame paused**, and **2.6997 ms** for
the one-time pause update. These are measurements on this machine, not test thresholds.

Untimed reference checks compare current positions, previous positions, normals and exact
UVs after forward/reverse playback, loop boundaries, seeks, and pause transitions. The real
lab's maximum position difference was 4.76837158e-6 and maximum normal difference was
2.98023224e-7. Affine blending reassociates floating-point operations, so results are not
bit-identical. Synthetic checks also cover morphing, non-unit/zero/invalid joint influences,
shared skins under different nonuniform mesh transforms, reversed triangle order, and
large primitives split across workers. Identical rendered images require the GPU checks below.

Performance modules: `animation.*` owns output mapping, cached positions and persistent workers;
`animation_pose.*` prepares shared poses; `animation_affine.h` and `animation_deform.h` implement
the skinning kernel; `animation_source.h` isolates the shared loader include; `tri_vertex.h`
defines the CPU/GPU vertex layout. Scene and acceleration modules gate uploads/refits, while
application/menu timing reports both CPU animation and complete wall-clock frame time.

Files changed for this performance work:

- Added `animation_pose.h/.cpp`, `animation_affine.h`, `animation_deform.h`,
  `animation_source.h`, and `tri_vertex.h`; replaced the internals of `animation.h/.cpp`.
  These separate pose preparation, deformation, vertex layout and worker coordination.
- Updated `scene.h`, `scene.cpp`, `scene_animation.cpp`, `accel.h/.cpp`, and `pathtrace.cpp`
  to bind direct output and skip unchanged uploads and acceleration updates.
- Updated `application.cpp`, `device.h`, and `menu.cpp` for CPU averages and wall timing.
- Added `tests_animation_perf.cpp` and `tests_animation_baseline.h`; expanded
  `tests_animation.cpp` and `tests.cpp`; updated `build.cmd` and this README.
- Updated `../bench/pw_gltf.h` only to cache the tracing environment flag instead of
  querying it for every primitive. Its deformation APIs and legacy numerical path are unchanged.

GPU verification still required: animated opaque/masked intersections and shadows; skin/morph
normals and loop seams; dynamic motion vectors under fixed and moving cameras; paused convergence;
follow camera and manual takeover; refit/rebuild timing and quality over long runs; correct picking
and emission on dynamic surfaces; SR/RR temporal behavior; and a clean D3D12 debug-layer run.
Compare deterministic playing, paused, seek and loop-boundary frame dumps against the legacy renderer.

Motion convention regression: `--motion pixels` (default) stores current-to-previous pixel displacement.
`--motion ndc` stores displacement divided by `(-renderWidth/2,+renderHeight/2)` and supplies those
scales to both SR and RR. Use `--depth reverse --width 3840 --height 2160 --render-scale 0.588`
to exercise the First Light scale convention. This is synthetic input, not a captured game buffer.

### Linear depth regression

`--depth linear-negative` emits negative linear view-space Z (metres, -300 for sky);
`linear-positive` emits its positive counterpart. Both declare linear depth to RR.
The projection and motion vectors retain the existing camera convention.
Use these modes with the fork depth-convention audit in addition to `standard` and `reverse`.


## One-core reference captures (2026.9.1)

Run the offline GPU benches sequentially from the repository root. Build with
`cmake --preset windows-x64`, the existing `windows-x64-release` build preset,
and `tools/bench12/build.cmd`. The plan's `windows-x64` build preset does not exist.
The source shader directory must contain exactly 21 DXBC files; remove only stale
`temporal_*_ps.dxbc` files before rebuilding.

```powershell
powershell -File tools/bench12/reference_dumps.ps1 -Runtime run_addon
powershell -File tools/bench12/reference_dumps.ps1 -Runtime run_r521
powershell -File tools/bench12/reference_dumps.ps1 -Runtime run_nrhost
python tools/bench/compare_reference.py --pair tools/bench12/run_addon/reference_before_2026.9.1/warp_dump_120.bmp tools/bench12/run_addon/reference_before_2026.9.1/warp_repeat_dump_120.bmp
powershell -ExecutionPolicy Bypass -File tools/bench9/run_matrix.ps1 -Only notemp,plain,game -CapturesRoot tools\bench9\run\reference_before_2026.9.1
powershell -ExecutionPolicy Bypass -File tools/bench9/run_matrix.ps1 -Only notemp -CapturesRoot tools\bench9\run\reference_before_2026.9.1_repeat
python tools/bench/compare_reference.py tools/bench9/run/reference_before_2026.9.1/notemp tools/bench9/run/reference_before_2026.9.1_repeat/notemp
```

Before bench9, copy the built add-on into `tools/bench9/run/host64/`, replace only
its `optimizer-fps-dlss5/*.dxbc` files with the 21 built shaders, and verify the count.
The bench12 runner performs the equivalent deployment itself.

`reference_dumps.ps1` accepts `-Runtime NAME`, `-Case warp,warp_repeat`, `-Out DIR`,
`-Build DIR`, and `-Frames 240`. It records frames 120 and 239 (use at least 240 frames).
Each runtime has its own `reference_before_2026.9.1` directory. The manifest records
HEAD, dirty status, SHA256 of source files and deployed binaries/shaders, exact commands,
exit codes, required log markers, per-case ini filenames and both dump-presence flags.
Each case also retains stdout, stderr and ReShade.log. Missing dumps invalidate a case.
The fixed layout is CenterX/Y=80, WorkX/Y=90, GlobalScale=100, ColorFilter=1;
ModelPasses=1, SpreadPasses=0 and TemporalEvery=4. Runtime-specific remaining settings
are preserved in the captured ini. No diagnostic environment variables are used.

Never overwrite references: a nonempty `-Out` is rejected. Later comparisons must use
another output directory, for example `-Case warp -Out task2`. All reference directories
are ignored by Git. No snapshot commit, `before-2026.9.1` tag or fork snapshot branch
was created during this capture task; references describe the dirty working tree.

Compare every `*_repeat` pair at both captured frames. T0/P1 requires byte equality;
zero MAD with different file bytes still fails. Temporal checks use an explicit
`--mad-limit 0.05` (RGB units 0..255); record measured MAD as well as exit status.
The comparator returns 0 for equality/allowed MAD, 1 for a difference and 2 for missing,
unreadable or differently sized images. Its six self-tests are registered as
`ofps_compare_reference`; missing NumPy/Pillow yields CTest skip code 77.

D3D11 capture is explicitly skipped on this workstation: the runtime has no
`tools/bench/run/dlss5-dx11-bridge.addon64`. Its reference directory exists but contains
no valid reference images. Do not treat an absent capture as a passing comparison.

Capture provenance limitation: `cmake --build --preset windows-x64` returned 1 because
that build preset is absent. Unless a fresh build is subsequently confirmed, the captured
add-on is the pre-existing build artifact (its SHA256 is in each manifest), not a verified
rebuild of the manifest's source tree. The bench12 executable itself was rebuilt successfully.

### Capture results, 2026-09-22

- Initial CTest: 9/9 passed. Final CTest: 10/10 passed, no skips, using existing C++
  test binaries. Both literal build-preset attempts failed (exit 1); a clean `/WX`
  add-on rebuild has not been established. `build.cmd` and its CPU checks passed (exit 0).
- `run_addon`: 7 main cases; `run_r521`: 9; `run_nrhost`: 13. Every case exited 0,
  had both dumps and the required log markers. Each runtime received 21 DXBC files.
- All three T0/P1 repeat pairs at frames 120 and 239 are byte-identical (MAD 0).
- Temporal repeats in `reference_before_2026.9.1_temporal_repeat`: `warp_t1` and
  `warp_forward_t1` for run_addon, `native_warp_t1` for run_r521, `warp_t1` for run_nrhost.
  All four runs exited 0; all eight image pairs are byte-identical (MAD 0).
- bench9 `game`, `notemp`, `plain`, repeated `notemp` and repeated `game` all exited 0.
  Each produced frames 600/650/700. The notemp warped marker and game temporal marker
  each occurred once. The host shader directory contains exactly 21 DXBC files.
- bench9 notemp repeat: comparator exit 1, MAD 1.4225 / 1.4230 / 1.4225.
  These captures are **not valid T0/P1 references**.
- bench9 game repeat: comparator exit 1 with `--mad-limit 0.05`,
  MAD 10.7396 / 10.7391 / 10.7397. These captures are **not valid temporal references**.
  No deeper bench repairs were attempted. D3D11 was skipped because its bridge is absent.

The ignored reference directories retain `manifest.json`, `repeat_comparison.json`
and `temporal_repeat_comparison.json` with full-precision results and provenance.
Repeat temporal captures with the same `-Case` list in a new `-Out` directory, then run:

```powershell
python tools/bench/compare_reference.py tools/bench12/run_addon/reference_before_2026.9.1 tools/bench12/run_addon/reference_before_2026.9.1_temporal_repeat --pattern '*t1_dump_*.bmp' --mad-limit 0.05
python tools/bench/compare_reference.py tools/bench9/run/reference_before_2026.9.1/game tools/bench9/run/reference_before_2026.9.1_repeat/game --mad-limit 0.05
```

## Direct shared core host

Build the core with `cmake --preset windows-x64` and
`cmake --build --preset windows-x64-release`, then run `cmd /c tools\bench12\build.cmd`.
Use `--host core --nr native --upscaler sr --nr-colour srgb` with
`--core-mode 0|1|2` (default 2) and `--core-temporal 0|1` (default 0).
The default `--host ngx` retains the existing NGX path.

The core host loads `optimizer-fps-dlss5-core.dll` beside the executable, with
21 DXBC files in `optimizer-fps-dlss5/` beside that DLL. It requires a clean runtime
without ReShade's local DXGI proxy or any loaded `.addon64` module.
`NrColourBridge` still encodes sRGB before the core and resolves it afterwards;
the model host's codec at the core boundary is identity. Model calls use the
existing `nvngx.dll_pwbench12.dll` forwarder. Queue submissions are reported
immediately after execution; shutdown drains the feature before destroying callbacks.

Final NGX blocks are saved to `ngx_final_audit.log` immediately before model calls.
The executable's optional `OfpsBenchAuditNgx` diagnostic lets the ReShade path
print its final block too. Audit excludes the shell-only `PeripheralWarp.FloatProbe`
float-slot diagnostic. Raw logs retain every other key, exact setter type, scalar
value and resource description by role; resource contents require separate GPU
verification. The owner-approved Off/pass-through exception permits only equal-value
`DLSSNR.Width`/`DLSSNR.Height` `uint32` versus `int32`: native Create is forwarded
unchanged, whereas the direct host uses the warped shell setter type. This also
covers those stored dimensions in later Evaluate blocks. Values must remain equal
and representable as nonnegative int32; changed sizes remain blockers.
`compare_ngx_audit.py` applies the exception only to `off_t0` versus `core_off`.
Resource pointer setters (`d3d12`/`pointer`) are compared by role and full description.
Other scalar type, key or value mismatches block parity even with identical dumps.
The audit does not modify the parameter block. Core effective settings are logged
as schema IDs and exact value bits.

The reference profile explicitly uses `Flags=0`, including in the direct host.
The schema default is `Flags=1` (extend motion beyond the frame edge); inheriting
it changes packed motion when the camera starts moving even though every final
NGX parameter still matches. Shutdown logs `OfpsStatus.fallbackFrames` and the
`submissionDrops` status row; the latter avoids changing the public status ABI.

Reference cases in `run_nrhost/reference_before_2026.9.1` map as follows:

| Core case | Reference case | Acceptance |
| --- | --- | --- |
| `core_warp` | `warp_t0` | byte-identical |
| `core_off` | `off_t0` | byte-identical, documented audit exception |
| `core_warp_t1` | `warp_t1` | RGB MAD <= 0.05 |

Audit both paths before comparing frames. T0 must be bit-identical; only temporal
uses `compare_reference.py --pair REFERENCE CANDIDATE --mad-limit 0.05`.

Run the direct matrix in an isolated allowlist runtime with:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/bench12/reference_dumps.ps1 -Runtime run_nrhost -Case core_warp,core_off,core_warp_t1 -Out candidate_p2t5
```

The runner copies audit logs beside the dumps. For a fresh audit of the shell path,
run `-Case warp_t0,off_t0,warp_t1` with a different output directory. Do not replace
`reference_before_2026.9.1`. After the final NGX audit passes, a T0 comparison is:

```powershell
python tools/bench/compare_reference.py --pair tools/bench12/run_nrhost/reference_before_2026.9.1/warp_t0_dump_239.bmp tools/bench12/run_nrhost/candidate_p2t5/core_warp_dump_239.bmp
```

Both runners accept a PowerShell `-DumpFrames` array (default `120,239`). For
time localization, run each path into a separate new directory, for example:

```powershell
& tools/bench12/reference_dumps.ps1 -Runtime run_nrhost -Case core_warp -Out candidate_timeline -DumpFrames ((1..23 | ForEach-Object { $_ * 10 }) + 239)
& tools/bench12/reference_dumps.ps1 -Runtime run_nrhost -Case warp_t0 -Out reference_timeline -DumpFrames ((1..23 | ForEach-Object { $_ * 10 }) + 239)
```

`build.cmd` also runs parser tests and the separate WARP session lifecycle test
(repeated Open/Close, changed settings, failed load and event cleanup), without
adding to the main 21/10 CTest suites.

## Runtime staging and release payload

`deploy_addon.ps1 -Build <build>` and `deploy_nrhost.ps1 -AddonBuild <build>`
stage the addon, core DLL, NGX forwarder and all 21 DXBC from the same build.
`tools/bench9/deploy.ps1 -Build <build>` stages that set in `host64/`.
Core version must match `cmake/Version.cmake`; stale DXBC are removed only
inside the selected runtime shader directory. Existing INI files are preserved.
`deploy_fork.cmd` preserves its source selection and INI, but refreshes core and
shaders together. This does not enable the legacy OptiScaler core integration.

Core cases run in `<Out>/core-runtime/`, copied by an allowlist with no ReShade,
local DXGI or addons. The manifest records actual argv, working directory and
binary hashes. All 45 effective schema settings are checked against the reference
INI (schema defaults apply to missing keys) before image comparison. Legacy keys
outside ABI 1's schema are not treated as active settings.

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/bench12/reference_dumps.ps1 -Runtime run_nrhost -Case core_warp,core_off,core_warp_t1 -Out candidate_p2t6
powershell -NoProfile -File tools/Package-Release.ps1 -Out dist-release
powershell -NoProfile -File tools/Test-ReleasePackage.ps1 -Zip dist-release/Optimizer-FPS-for-DLSS5-2026.9.1.zip
```

The ZIP and CI artifact contain x64 addon/core/forwarder, 21 shaders next to
core, and the x86 remote addon. ZIP verification checks every payload hash against
`files.sha256`. This is a bench validation package; compatibility of the older
installer with the new core payload has not been established.

## Plan 2 Task 7 verification (2026-09-22)

Reused the existing `candidate_p2t7` captures from source
`2c29f6dafa25485cb177ee25e6bd44741d5a23ed`; no NR runtime was relaunched and no
reference was replaced. Core captures are in `run_nrhost/candidate_p2t7/core/`.
Manifests retain exact argv, source/binary/shader hashes and capture status.
The four spatial DXBC differences from the original reference were already present
in accepted plan-1 `candidate_task10`; all 21 match that baseline. NR/SR/RR hashes
match the original reference. These are capture results, not fresh GPU runs of the
final rebuilt package.

| Runtime | Cases | T0 pairs, byte-identical | T1 pairs, MAD <= 0.05 | Maximum MAD |
| --- | ---: | ---: | ---: | ---: |
| `run_addon` | 7 | 10/10 | 4/4 | 0 |
| `run_r521` | 9 | 16/16 | 2/2 | 0 |
| `run_nrhost` | 13 | 24/24 | 2/2 | 0 |
| direct core | 3 | 4/4 | 2/2 | 0 |

All 64 pairs (frames 120 and 239) are byte-identical, including T1. Final NGX
audit matches 240 `core_warp`, 240 `core_off` and 61 `core_warp_t1` blocks under
the documented policies. Off has 480 equal-value dimension setter exceptions;
the warped paths have none. All three core logs report zero `fallbackFrames`
and `submissionDrops`. Resource contents were not independently captured at the
NGX boundary; final-frame equality does not establish full input-buffer identity.

Reproduce comparisons by folder, using each reference manifest's temporal setting
and the explicit core mapping, without applying a global MAD tolerance to T0:

```powershell
python tools/bench12/test_compare_ngx_audit.py
python tools/bench12/compare_plan2.py --candidate candidate_p2t7
```

The driver first audits the three core paths, then compares every reference case.
It writes `frame_comparison.json` in each candidate directory and
`ngx_audit_comparison.json` in the nrhost candidate. It never launches a bench.
Equivalent explicit core comparisons are:

```powershell
$root = 'tools/bench12/run_nrhost'
foreach ($pair in @(@('warp_t0','core_warp'), @('off_t0','core_off'), @('warp_t1','core_warp_t1'))) {
    foreach ($frame in @(120,239)) {
        $compareArgs = @('tools/bench/compare_reference.py', '--pair',
            "$root/reference_before_2026.9.1/$($pair[0])_dump_$frame.bmp",
            "$root/candidate_p2t7/core/$($pair[1])_dump_$frame.bmp")
        if ($pair[0] -eq 'warp_t1') { $compareArgs += @('--mad-limit','0.05') }
        python @compareArgs
        if ($LASTEXITCODE -ne 0) { throw 'Core reference mismatch' }
    }
}
```

bench9 has no `candidate_p2t7` capture. Existing `candidate_task10` retains three
dumps each for `game`, `notemp`, and `plain`: warped markers occur once in game
and notemp, temporal-ready once in game, and NR evaluation succeeds in all three.
This is historical smoke evidence, not a passed plan-2 run or image-parity result;
the original bench9 repeats were nondeterministic. New bench runs were prohibited.
D3D11 is BLOCKED: `tools/bench/run/dlss5-dx11-bridge.addon64` is absent.
`run_fork` is NOT PASSED: the older integration can double-warp without the event.

Final local verification logs are in ignored `out/validation-p2t7/`: all five
Release presets build with `/WX`; CTest passes 21/21 x64, 10/10 x86 and 7/7 SDK-only
without skips. Shipping MT and remote presets intentionally have no tests.
bench12 `/WX`, CPU/parser checks and WARP lifecycle tests pass. The six audit
comparison tests reject changed sizes, wrong scalar types and missing keys.
Core include boundaries pass, the ABI header diff is empty, and PE inspection
finds exactly two C exports and no dynamic CRT dependency.

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/Lint-PowerShell51.ps1 -Path tools
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/Package-Release.ps1 -Out dist-release -Force
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/Test-ReleasePackage.ps1 -Zip dist-release/Optimizer-FPS-for-DLSS5-2026.9.1.zip
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/installer-tests/Run-InstallerTests.ps1 -Payload dist-release/Optimizer-FPS-for-DLSS5-2026.9.1/payload -BenchDir tools/bench12/run_addon
```

Lint passes under Windows PowerShell 5.1. ZIP verification passes with all 26
payload hashes and 21 DXBC. Installer self-tests are reported separately from
package validation; passing payload hashes does not establish installer migration.
After updating the two old file-count assertions for the optional core payload and
adding core hash/receipt/host64 checks: **73 passed, 0 failed, 2 skipped**.
The skipped manual-adoption and pre-26.26 migration scenarios need the absent
`peripheral-warp.addon64.pre2625` fixture. The run used
`-ReShade32 out/vulkan-smoke-2c614b7ed0dc459f8dd85489a5fc3e05/ReShade32.dll`.
These checks do not certify complete core-aware Verify/migration/unload support.
