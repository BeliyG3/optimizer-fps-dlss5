# Background temporal port: clean GPU comparison, 2026-09-20

**Selected background defaults: expected depth ON, model-motion validation ON, phase-in OFF.**
Version remains 26.28. The installed 26.28 package predates the background fix; this is an
unpublished replacement of that same version. No commit, push, download, game launch or edit to
`shaders/temporal.hlsl` was performed.

Final payload verification and rollout status are recorded at the end of this report.

## Method and limits

HEAD before this work was `dadc31d`. Builds used PowerShell and the prescribed CMake presets;
`run_modes.sh` ran from Git Bash in `tools/bench`, with the supplied `BENCH_RUN`. Each deployment
copied the add-on, forwarder and all 15 DXBC files together. Initial GPU usage was 2,085 MiB of
16,376 MiB; sampled bench usage was about 7,100–7,150 MiB, rather than the previous 15,312 MiB.
No competing application was closed. The previous contested report is archived locally as
`out/background-previous-20260919.md`; its numbers did not decide these defaults.

All quality rows below contain **three runs**, 232 frames each, cap 60 FPS, dumps 200–219,
background `3,2`, Mode=2, Peripheral 80/90, cells enabled and unchanged bench settings including
`DebugLayer=1`. Forward flight adds `BENCH_EXTRA="--camera forward"`. The initial eight-configuration
matrix rotates configuration order within each repetition. The additional control and candidate
groups alternate scenes. There are 72 accepted quality runs across the matrix and rechecks.

Values are **median [minimum, maximum]** of `errflick.py`'s luma MAD summaries for frames 201–219,
in 0–255 units. C is the central 80% of width and height; P is the remaining area. Change is the
MAD of the change in the error map, removing the reference scene's own motion. Lower is better.
Ranges are observed scheduling variation, not confidence intervals. Mean-error gains are not a
claim that every asynchronous run beats every baseline run. These are quality measurements,
not a controlled throughput comparison between feature combinations.

Every row uses one fixed same-scene `0,2` reference: `clean_ref_default_1` or
`clean_ref_forward_1`. All default references were identical. Forward references 1 and 2 were
identical, but reference 3 reproduced the other historical output variant (`bg_reg_fwd_every`);
references 1 and 2 reproduce `fwd_every`. At frame 210 the two variants differ by RGB MAD
0.455/0.513. This variability existed before these changes; its underlying startup cause remains
unresolved. No run-specific reference was chosen to improve a metric. Fixed-reference byte checks
across builds and the synchronous checks are listed in the final verification section.

Switches are `DebugTemporalNoExpect`, `DebugTemporalPhaseIn`, `DebugTemporalNoModelMotion`:
baseline = `1,0,1`; expected only = `0,0,1`; phase only = `1,1,1`; model only = `1,0,0`;
all on = `0,1,0`. The selected combination is `0,0,0`. An explicit phase value of 1 is the
automatic length at `3,2`; an explicit -1 still selects the automatic length in both paths.

## Full feature matrix

| Scene / configuration | Error C | Error P | Change C | Change P |
| --- | --- | --- | --- | --- |
| default / Baseline: all off | 3.654 [3.586, 4.374] | 3.333 [3.244, 4.214] | 0.952 [0.945, 1.013] | 1.099 [1.057, 1.185] |
| default / Expected depth only | 3.790 [3.267, 4.079] | 3.477 [3.112, 3.877] | 0.954 [0.946, 0.988] | 1.063 [1.020, 1.107] |
| default / Phase-in only | 3.741 [3.317, 4.305] | 3.319 [3.227, 3.830] | 0.935 [0.898, 0.955] | 1.043 [0.974, 1.093] |
| default / Model motion only | 3.613 [3.043, 3.992] | 3.671 [3.257, 3.950] | 0.955 [0.917, 1.064] | 1.092 [1.052, 1.274] |
| default / All three on | 3.341 [3.087, 3.452] | 3.283 [3.264, 3.729] | 0.898 [0.891, 0.917] | 1.059 [1.018, 1.066] |
| default / Expected depth + phase-in | 4.267 [3.650, 4.308] | 3.821 [3.179, 3.899] | 0.962 [0.921, 0.967] | 1.089 [1.033, 1.093] |
| default / Phase-in + model motion | 3.210 [3.184, 3.252] | 3.239 [3.212, 3.269] | 0.908 [0.877, 0.912] | 1.040 [1.011, 1.086] |
| default / Expected depth + model motion | 2.901 [2.660, 3.392] | 2.961 [2.701, 3.589] | 0.920 [0.892, 0.945] | 1.023 [1.003, 1.081] |
| forward / Baseline: all off | 3.431 [2.955, 3.855] | 3.229 [3.228, 3.684] | 0.664 [0.651, 0.691] | 0.654 [0.641, 0.707] |
| forward / Expected depth only | 3.495 [3.249, 3.618] | 3.534 [3.282, 3.540] | 0.676 [0.672, 0.686] | 0.679 [0.658, 0.683] |
| forward / Phase-in only | 3.455 [3.167, 3.496] | 3.302 [3.101, 3.309] | 0.648 [0.640, 0.654] | 0.630 [0.619, 0.638] |
| forward / Model motion only | 3.342 [3.002, 3.441] | 3.122 [2.983, 3.196] | 0.667 [0.645, 0.673] | 0.664 [0.647, 0.673] |
| forward / All three on | 3.233 [3.071, 3.423] | 2.998 [2.852, 3.207] | 0.640 [0.629, 0.640] | 0.632 [0.615, 0.633] |
| forward / Expected depth + phase-in | 3.086 [3.044, 3.231] | 3.144 [3.112, 3.218] | 0.638 [0.637, 0.647] | 0.619 [0.616, 0.631] |
| forward / Phase-in + model motion | 3.237 [3.117, 3.446] | 3.211 [3.001, 3.224] | 0.640 [0.630, 0.644] | 0.633 [0.623, 0.659] |
| forward / Expected depth + model motion | 3.234 [3.232, 3.420] | 3.014 [2.997, 3.170] | 0.655 [0.645, 0.664] | 0.657 [0.637, 0.668] |

Raw directories: `clean_<off|expect|phase|model|all|nomm|noexpect|nophase>_<default|forward>_<1|2|3>`.

## Rechecks and final selection

| Scene / configuration | Error C | Error P | Change C | Change P |
| --- | --- | --- | --- | --- |
| default / All-on defaults, before switch fix | 3.306 [2.981, 3.425] | 3.184 [3.097, 3.458] | 0.899 [0.893, 0.902] | 1.032 [1.017, 1.034] |
| default / All-on defaults, device fix | 3.215 [2.963, 3.344] | 3.199 [3.193, 3.385] | 0.906 [0.893, 0.923] | 1.045 [1.032, 1.066] |
| default / Fresh all-off control | 4.297 [2.789, 4.306] | 4.146 [2.900, 4.178] | 0.976 [0.912, 0.991] | 1.110 [0.975, 1.137] |
| default / Selected: expected depth + model motion | 3.191 [3.190, 3.263] | 3.280 [3.162, 3.619] | 0.935 [0.910, 0.937] | 1.065 [1.057, 1.066] |
| forward / All-on defaults, before switch fix | 3.392 [3.028, 3.507] | 3.156 [2.875, 3.255] | 0.643 [0.625, 0.645] | 0.632 [0.618, 0.637] |
| forward / All-on defaults, device fix | 3.401 [2.747, 3.564] | 3.243 [2.640, 3.261] | 0.642 [0.624, 0.645] | 0.636 [0.612, 0.636] |
| forward / Fresh all-off control | 3.321 [2.918, 3.505] | 3.136 [2.805, 3.408] | 0.666 [0.649, 0.669] | 0.647 [0.635, 0.653] |
| forward / Selected: expected depth + model motion | 2.959 [2.798, 3.107] | 2.823 [2.676, 2.976] | 0.647 [0.638, 0.660] | 0.646 [0.633, 0.672] |

Raw directories: `clean_<default|fixed|control|candidate>_<default|forward>_<1|2|3>`.
The first two rows per scene intentionally preserve the all-on repeat results that weakened its
initial apparent mean-error win. They were not discarded as inconvenient noise. `fixed` includes
the device restoration fix; `candidate` also includes the pending-input retirement fix. Neither
transition fix changes the shaders or the steady-state temporal equations.

* **Expected depth ON.** With phase-in off, adding it to model motion improves initial matrix
  error from 3.613/3.671 to 2.901/2.961 in the default scene and from 3.342/3.122 to 3.234/3.014
  in forward flight. Expected depth alone was not a win; the combination is what earns its place.
* **Model motion ON.** Adding it to expected depth, without phase-in, improves initial default
  error from 3.790/3.477 to 2.901/2.961 and forward error from 3.495/3.534 to 3.234/3.014.
  The old provisional OFF choice is replaced by this measured result. It is not universally
  better in every combination: with the fade on, raw model vectors had lower forward centre
  error but worse default error and forward periphery error.
* **Phase-in OFF in background.** Initial expected-depth + model-motion error was 2.901/2.961
  default and 3.234/3.014 forward. Adding phase-in gave 3.341/3.283 and 3.233/2.998 respectively:
  no meaningful forward mean-error gain, and higher default mean error. Its forward change
  improved from 0.655/0.657 to 0.640/0.632, but all-on failed to retain a forward mean-error win
  in the later control comparison. The fade did not earn the shipped default. It remains opt-in.

The selected repeat has default error **3.191/3.280 versus 4.297/4.146** for the fresh all-off
control (25.7%/20.9% lower), and forward error **2.959/2.823 versus 3.321/3.136** (10.9%/10.0%
lower). It also improves over the original baseline median (3.654/3.333 default,
3.431/3.229 forward). Forward error-map change is 0.647/0.646 versus 0.666/0.647: centre
improves, periphery is essentially unchanged. The observed ranges overlap; this is a repeated
median quality win, not a worst-case bound. Pooling the two independent three-run groups for
baseline and the selected combination still lowers forward median error by about 6.1%/7.5%.

The default is implemented independently of synchronous phase-in. With diagnostic keys absent,
background phase-in is zero; synchronous phase-in remains automatic. Explicit 0, positive and
negative phase overrides remain available. No user INI needs a diagnostic key added or changed.

## Transition defects found during verification

The first 1,800-frame switch run (`clean_final_switch`) exposed a real fallback at frame 300:
layout recreation retired the device aliases, then the native path checked host queues before
restoring those aliases. `EnsureTemporalDevice` now restores them before mode selection. The
real no-host-queue fallback is preserved; the fix does not bypass queue availability checks.

The next switch run (`clean_fixed_switch`) had no add-on fallback, but a pending input signal
caused a three-second retirement timeout. The job had an observed submission queue, while the
retirement registry lookup used only the real device identity. Retirement now signals that
observed queue just like the normal evaluate path. The existing fence-gated retirement remains
in place. The failed/intermediate logs are preserved, including the 3,220.80 ms frame outlier.

## Files and reproduction

* `adapters/reshade/diagnostics.h`, `addon/config_store.cpp` and `ngx/async_scheduler.cpp`:
  independently measured background defaults and explicit phase overrides.
* `ngx/temporal_controller.h/.cpp` and `ngx/hook_dispatch.cpp`: restore device aliases before
  the first post-recreation background queue check.
* `ngx/async_scheduler.h`: release a pending input wait on its observed host submission queue.
* `CHANGELOG.md`, `docs/RESHADE_ADDON.md`, `docs/dev/NGX_MODULES.md` and this report:
  defaults, evidence, same-version update explanation and remaining limits.

The larger existing dispatch/controller files retain their responsibilities; this change adds
one device-initialization helper and reuses it rather than creating a new orchestration layer.
The GPU switch bench reproduces the two transition defects; the normal eight-test CTest suite
also remains required. Local scripts/logs are `out/background-clean*`, with the aggregate values
in `out/background-clean-results.json`. Each runtime folder contains its INI, ReShade log, dumps
and `metrics.txt`; attempt output is beside the folder. All accepted bench runs completed on
their first attempt; no D3D11 bridge `[fail]` retry was needed. One harness continuation was
interrupted by editing its script while it ran; the six completed quality runs were preserved
and its regression checks were rerun from a separate script.

```text
python tools/bench/errflick.py <fixed-reference> <run> 200 219
ctest --test-dir out/build/x64 -C Release --output-on-failure
```

Protected shader SHA-256 (repository and fork):
`EAE331968B80608A818B2C900527E1E23AA9FB86202FF51BA42D1D75E45CEBC5`.

## Final verification and rollout

* Final `windows-x64-release`, `windows-x64-mt-release` and `windows-x86-remote-release` builds
  passed without compiler/linker warnings. Final CTest: 8/8 passed.
* The static-CRT shipping payload reproduces all 20 `clean_sync4` and all 20 `clean_sync8`
  dumps from the fresh `dadc31d` build. Some intermediate sync runs differed; those runs were
  retained, and the final shipping comparison is recorded in `out/background-clean-shipping-checks.json`.
* `clean_ship_ref_default` matches all 20 `clean_ref_default_1` frames; `clean_ship_ref_forward`
  matches all 20 `clean_ref_forward_3` frames, proving reference parity across builds for both scenes.
* `clean_ship_switch`: 1,800 frames, `--switch 300 --frametime`, actual shipped defaults with
  all three diagnostic override keys absent. All 1,790 async evaluates from 10 through 1799 are
  present; inferred adoption counts in consecutive 300-frame windows are 76, 80, 87, 82, 72, 86.
  The final observed adoption is frame 1797. No add-on fallback, device removal, exception,
  nonzero async failure counter or three-second retirement timeout occurs.
* Switch frametime summary after warmup: 1,740 frames, mean 17.42 ms, p99 16.68 ms, maximum
  297.55 ms. Layout/model recreation still produces transition spikes; this is not a zero-stutter claim.
* RenoDX still prints its startup `restored full host compute state after direct NR fallback`
  message before background activation. That is not an add-on fallback frame. Existing third-party
  startup hook diagnostics, missing optional ReShade shader-search-path warnings and unbound-output
  debug-layer warnings are not represented as a completely warning-free runtime.

* `tools/Package-Release.ps1` rebuilt `dist-release/Optimizer-FPS-for-DLSS5-26.28.zip` from the
  final static-CRT x64 build and x86 remote build. ZIP SHA-256:
  `1977f93821a15c23bc2f9326e520adf502344e254b543c038230ba2d15e63849`.
* **Rollout completed: 44 installations / 52 payload folders.** The inventory found 41 current
  receipts, one belonging to excluded `007 First Light`; the other 40 were updated with the
  installer using `-Mode Update -Yes -NoPause`. Four manual installations were updated:
  Remember Me (host64 and its 32-bit tab), Watch Dogs 2, Detroit Become Human and No Man's Sky.
  The eight remote-tab folders account for the difference between installations and folders.
* Each game/process was checked immediately before its update. None was launched. Installer exit
  code 10 means installation succeeded but a new game run is still needed for runtime evidence;
  this expected condition was accepted only alongside direct file and INI verification.
* All 756 payload file hashes match the package: 44 add-ons, 44 forwarders, 660 DXBC files and
  eight remote tabs. Shader filename sets also match exactly. All 126 captured INI files are
  **byte-identical**, which also preserves every existing user key.
* Manual old payloads are preserved in each applicable folder's
  `_OptimizerFPS/manual-26.28b-20260920`, including Remember Me's separate remote-tab backup.
  Installer-managed backups and updated receipts were produced by the installer.
* `007 First Light` and all Kits in `25_DLSS5` were not modified. The original bench INI was restored
  after measurement; per-run INIs remain in the archived run folders.

Per-installation verification is in `out/background-clean-rollout/verification.json`; its
`targets.json` and 126 INI snapshots preserve the pre-update evidence. Installer output is in
`install-0.log` through `install-39.log`. The previous package ZIP is retained locally as
`out/background-clean-pre-update26.28.zip`. No commit or push was made; HEAD remains `dadc31d`.

Remaining limits: installed-game gameplay/runtime compatibility is untested because games were
not launched; asynchronous scheduling noise and the pre-existing startup output variability remain.
The quality result covers these two procedural scenes and cadence `3,2`, not every game or cadence.
