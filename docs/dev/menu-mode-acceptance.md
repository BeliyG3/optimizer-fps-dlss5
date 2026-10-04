# Menu mode: stage-1 bench acceptance (D3D12)

Plan: `docs/superpowers/plans/2026-09-29-menu-mode.md`, Task 9 and correction C7. Yardstick: the spike
(`docs/dev/menu-mode-spikes.md`, 0b-1; the spike pages are not published). The menu frame goes to NR as it is, so the look
numbers are recorded, not tuned. No game install was touched.

**Build:** `feature/menu-mode` at `cc66b9c246705ecc5d68ee310d3b7f6a89aaae32` (Tasks 1-8), `out\build\x64`, add-on
`223E5BAF...30EB9C5`, core `9DD6BFAF...0A281C54` (SHA-256, from `%TEMP%\mm_menu\manifest.json`). Steps 1-2 ran the
bench12 exe of that commit, step 3 and C7 the bench12 exe with `--nr-recreate` / `--resize-at` (commit `780b890`;
the defaults are unchanged). Run 2026-09-29 on the bench PC used by the spike.

**Verdict: stage 1 passes on the bench.** Controller ruling (2026-09-29, Task 9 fix round 1): the `mp_model_matched`
in-menu threshold is about 44 dB, not the brief's 48 dB. The brief quoted the spike's variant without the exit Reset;
with the Reset the owner ruled in (2026-09-29) the spike measured 44.40-49.36 dB and the product 44.32-49.14 dB.

## Step 1: release gate with menu mode off

`reference_dumps.ps1 -Runtime run_nrhost -WarpPath pixel -Out %TEMP%\mm_gate -Case off_t0,warp_t0,warp_t1,uni_black`,
dumps hashed against `tools\bench12\run_nrhost\reference_before_2026.9.1`.

| Case | Expected | Measured |
|---|---|---|
| off_t0, warp_t0, warp_t1, uni_black (frames 120, 239) | 8 dumps byte-identical | 8/8 `True`, every case `ok` |

## Step 2: menu cases on bench12

`reference_dumps.ps1 -Runtime run_nrhost -Out %TEMP%\mm_menu -Case mp_off,...,mp_off_motion` (21 cases), then
`menu_psnr.py`, `menu_cpu.py` and the log checks exactly as in the Task 9 brief.

| Case | Expected | Measured |
|---|---|---|
| all 21 cases | `ok`, exit 0, `device ok` | all `ok`, exit 0 |
| mp_marker | red in menu 10/10, after exit 0/50 | 10/10, 0/50 (mp_marker_dbg the same) |
| mp_fail | red 0/..., forced-failure True | red 0/10, 0/50, forced-failure True |
| mp_sigfail | one stop, stop frame untouched | stop at present 73, untouched True, no entry after it |
| mp_exit | process ends inside a menu, exit 0 | one run entered (176 ms), no exit line, exit 0 |
| mp_model | enter 10 exit 10, stop -1, min enter >= 150 ms | 10 / 10, -1, 157 ms |
| mp_model in menu vs every-frame NR | 33.5-36.0 dB, above the untreated pause | 34.59-35.12 (mean 34.83); pause 29.51-29.83 |
| mp_model first 5 after exit vs mp_off_model | >= -6.7 dB | worst -5.68 dB (spike with Reset -5.68..-3.25) |
| mp_model_matched in menu | ~44 dB (ruling; brief: >= 48, spike without Reset 48.78-49.41) | 44.32-49.14, falling from run 1 (49.14) to run 7 (44.32); spike with the exit Reset: 44.40-49.36 |
| mp_model_matched after exit | >= -6.7 dB | worst -6.17 dB (spike with Reset -6.17) |
| mp_model_cl | within 0.5 dB of mp_model, run by run; log line | max paired difference **0.15 dB** (run 6: 34.84 at present 374 vs 34.69 at 373; each against its own every-frame reference); 6 of 10 runs dump the same present and give the same dB. Rerun (`%TEMP%\mm_fix1`) against the same mp_model: max 0.10 dB. `the host runs NR on its own queue ...; menu passes are fenced against it` |
| mp_model_motion | recorded only | in menu 28.72 min, 30.80 mean (pause 24.95 / 26.02); after exit worst -2.07 dB |
| menu_cpu.py, pipeline without the evaluate | avg < 1 ms, p95 < 1 ms | avg 0.394, p95 0.642 (1st present of a run avg 1.211, max 2.852) |
| debug layer (mp_model_dbg, mp_marker_dbg) | 0 errors | 0 `D3D12 ERROR`/`CORRUPTION`; `.err.txt` has warnings only |

The matched numbers: the product resets NR history at the first host evaluate after a menu (owner ruling
2026-09-29); the spike measured that variant as `mp_model_matched` (44.40-49.36, 0b-1.md) and the no-Reset variant as
`mp_model_matched_noreset` (48.78-49.41), which is the number the brief quotes. The product reproduces the Reset variant
within 0.1 dB. The decline over runs is the Reset's cost in the colour-matched setup (shorter history before each
menu); the tone-mapped `mp_model` stays flat (34.59-35.12).

## Step 3: the own block on hosts that never pause

| Host | Check | Measured |
|---|---|---|
| run_r521 (renodx 5.2.1) | pp_trace, pp_own vs pp_ref | identical (2/2 each), `DebugMenuOwnBlock=1` line (35 keys, 5 absent) |
| run_addon (renodx 4.70) | pp_trace, pp_own vs pp_ref | identical (2/2 each), own-block line (35 keys, 5 absent) |
| run_nrhost | pp_trace, pp_own vs pp_ref | identical (2/2 each), own-block line (32 keys, 8 absent) |
| Dagherbou v0.2.0-patch1, D3D12 | params_own vs off | identical (2/2); script PASS; own-block line in its ReShade.log |
| Dagherbou, D3D11 bridge | d3d11_params_own vs d3d11_off | 120 identical, 239 54.94 dB (stand noise, as in the spike); PASS |
| wilsjo2 v0.8.91 (folder present) | params_own vs off | identical (2/2); script PASS; own-block line present |

renodx 5.2.1 watched feature: first the 960x540 one (`menu mode: watching feature ...`, then `Menu mode: unavailable:
the game's NR input is not the whole frame (a scaled or padded NR input) (retried automatically)`); after renodx
released it (present 35) the 1920x1080 feature (guides 960x540) became the watched one and its model pass was built.
Both devices equal the NR colour's device.

## C7: lifetime events while the game keeps running

New bench12 flags: `--nr-recreate F` (feature 18 released after the queue is idle, created again the same frame) and
`--resize-at F[,W,H]` (`ResizeBuffers` at the same size, or to WxH, which rebuilds renderer, DLSS and feature 18).
Cases `mp_recreate`, `mp_resize`, `mp_resize_size`: the event at frame 80, inside the first pause while its run is
under way, then 9 more pauses; `--debug-layer` on. The check (`Test-MenuLifetime` in `menu_check.ps1`, see
`tools/bench12/MENU_MODE.md`) requires the bench's line for the event at frame 80 plus the add-on's view of it
(`release 80` in `menu_events.log`, or ReShade's `ResizeBuffers` line), a menu run entered before 80 and ending at or
after it, and every later run with its own recorded pass (an in-menu dump at a present whose `t` line has a nonzero
evaluate recording). No-flag control: the same check on `mp_model_dbg` of step 2 (the same arguments without the flag)
and on `mp_model` fails with `event logged False` for both events.

```powershell
powershell -NoProfile -File tools/bench12/reference_dumps.ps1 -Runtime run_nrhost -Out $env:TEMP\mm_fix1 -Case 'mp_recreate,mp_resize,mp_resize_size,mp_ref,mp_model_cl,mp_ref_cl'
python tools/bench12/menu_psnr.py $env:TEMP\mm_fix1 mp_recreate mp_ref
```

| Case | Expected | Measured (`%TEMP%\mm_fix1`) |
|---|---|---|
| mp_recreate | event inside a run, no hang, exit 0, no debug error, every later menu gets a pass | exit 0, `device ok`; event logged, inside a run (64-80, ends `release 80`); 9 later runs, 9 with their own pass. First run (`mm_life2`): last pass f2 16 completed 16 at the release; in menu 34.59-35.17 dB vs mp_ref |
| mp_resize (same size) | same | exit 0; event logged, inside a run (the run goes on through `ResizeBuffers`: no drain, no rebuild; enter 10 exit 10); 9 later runs, 9 with their own pass |
| mp_resize_size (1600x900) | same | exit 0; event logged, inside a run; the bench re-creates feature 18 at 1600x900 (run ends `release 80`, rebuilt at 1600x900); 9 later runs, 9 with their own pass |
| debug layer, all three | no error | 9 messages each, all severity 2 (warnings); an error would have thrown (no `device ok`) |

None of the three stopped menu mode, quarantined, withheld an evaluate or logged a missed dump.

## Stage 3: layout (Task 12, Mode Uniform / Peripheral in menus)

A compressed model (Mode Uniform or Peripheral) no longer makes menu mode "unavailable": the menu frame goes through the
core's own feature evaluate (pack -> model -> unpack) on the private list, exactly like a game frame; a model at the
frame's size keeps the direct pass of stage 1. No temporal mode in these runs (the cadence is in "Stage 3: temporal" below). D3D12 only: on a
D3D11 swap chain a compressed model shows `unavailable: Mode Uniform or Peripheral in menus needs a D3D12 game for now`
until the bridge's queue is registered with the core (Task 10). Build: `feature/menu-mode`, Task 12 working tree on
`a7a01f1`, bench12 with `--nr-release-thread`. Each case is compared with its own every-frame reference (`mp_ref_uni`,
`mp_ref_per`) and its own pause without menu mode (`mp_off_uni`, `mp_off_per`), static camera, 10 menus.

```powershell
powershell -NoProfile -File tools/bench12/reference_dumps.ps1 -Runtime run_nrhost -Out $env:TEMP\mm_s3 -Case 'mp_model,mp_ref,mp_off_model,mp_model_cl,mp_ref_cl,mp_off_cl,mp_recreate,mp_resize,mp_model_uni,mp_ref_uni,mp_off_uni,mp_model_per,mp_ref_per,mp_off_per,mp_race_per'
python tools/bench12/menu_psnr.py $env:TEMP\mm_s3 mp_model_per mp_ref_per --off mp_off_per
powershell -NoProfile -File tools/bench12/reference_dumps.ps1 -Runtime run_nrhost -Out $env:TEMP\mm_s3_uni -Case 'mp_model_uni,mp_ref_uni,mp_off_uni,mp_marker_uni'
powershell -NoProfile -File tools/bench12/reference_dumps.ps1 -Runtime run_nrhost -WarpPath pixel -Out $env:TEMP\mm_gate3 -Case 'off_t0,warp_t0,warp_t1,uni_black'
```

| Case | Expected | Measured (`%TEMP%\mm_s3`, 2026-09-29) |
|---|---|---|
| all 15 cases | `ok`, exit 0, `device ok` | all `ok`; every model case enter 10 exit 10, no stop, no refused or skipped present |
| release gate (MenuMode=0) | 8 dumps byte-identical to `reference_before_2026.9.1` | 8/8 identical (`%TEMP%\mm_gate3`) |
| mp_model_uni: path | the core warps the menu frame | `menu core pass: the first menu frame went through the core (evaluate 0, path 1, warp path 1, model 0x00000001)` |
| mp_model_uni in menu | above the pause without menu mode | **36.62-37.20 dB (mean 36.82)**; pause without menu mode 27.36-28.02 (mean 27.56); rerun (`mm_s3_uni`) 36.44-36.74 |
| mp_model_uni after exit | >= -6.7 dB (brief) | worst **-9.84 dB** (mean after-exit 34.31 vs 42.90): **the exit Reset's own cost in Uniform**, see the control below |
| mp_marker_uni (control) | - | marker in menus (no model), same exit Reset: after-exit min 32.62, mean 34.31, worst -9.84 dB, the same numbers as mp_model_uni to 0.01 dB |
| mp_model_per in menu | above the pause without menu mode | **34.25-36.13 dB (mean 35.13)**, falling from run 1 to run 9; pause without menu mode 28.15-29.10 (mean 28.43) |
| mp_model_per after exit | >= -6.7 dB | worst **-5.91 dB** |
| mp_model (Mode Off, direct pass) | within 0.1 dB of Task 9 | 34.47-35.19, mean 34.87 (Task 9: 34.59-35.12, mean 34.83); after exit worst -5.68 dB (Task 9: -5.68) |
| mp_model_cl | as mp_model | 34.47-35.19; paired with mp_model run by run max 0.11 dB (run 6, a different present) |
| mp_recreate, mp_resize (C7) | as in Task 9 | both `ok`: event logged, inside a run, 9 later runs with their own pass; debug layer 0 errors |
| mp_race_per (C4) | release racing a menu call; no hang, later menus get passes | `ok`: the release ran on a second thread during present 80 of a Peripheral run; `the release of feature ... waited 1.62 ms for a present under way`, then `the release waited 13.1 ms for the last menu pass` (f2 18, completed 17); feature created again at frame 81; 9 later runs, 9 with their own pass; debug layer 0 errors |
| CPU per menu present (whole present, `DebugMenuDump=1`) | recorded | Off 0.544 ms avg; Uniform 0.646; Peripheral 0.812 (one 22.9 ms present at 497 in the pipeline's poll/report phase, the evaluate recording 0.40 ms there) |
| GPU per menu present (private list) | recorded | Off 5.02 ms avg; Uniform 4.40; Peripheral 4.45 |

The Uniform after-exit number is not a regression of the core pass: with the marker in menus (no model at all) the
first five frames after each exit are identical, so they are set by the exit Reset (owner ruling 2026-09-29) and NR's
restart at Uniform's 90% work size, which recovers more slowly than at the frame's size. Peripheral stays inside the
bound (-5.91). Controller ruling 2026-09-29: keep the exit Reset and relax the after-exit bound for compressed modes (without the
Reset the menu history would bleed into the game); listed under the in-game checks.

### Stage 3, fix round 1 (review C1/C2/I1)

Rules added before any menu call into the core. (1) A setting changed since the game's last evaluate means the run
ends with `unavailable: a setting changed in this menu; it applies from the game's next frame (retried
automatically)`. The snapshot records a settings epoch, and every `OFPS_EVENT_SETTINGS_CHANGED` that changes a value
other than MenuMode advances it. No menu call can then re-lay out the feature, re-create its model or retire its GPU
objects. If the core still needs a model inside a menu call, the creation is refused, `RequestModelRebuild()` is
called, and menus stay untouched until the game's next evaluate re-creates the model and its extra passes.
(2) No core call starts while an earlier menu pass is still on the GPU. This is a non-blocking check that f2 has
reached its last signalled value. Such a present shows the last pass's output again; before the run's first pass it
leaves the frame untouched. (3) With TemporalMode != 0 and a compressed model, the menu was blocked in this round, because
`OfpsFrameInputs` has no "force a full frame" input and `hostReset` would also reset NR's history on every menu frame.
Task 13 removed this rule (see "Stage 3: temporal"). The bench runs are in `%TEMP%\mm_f1`, and the release gate in `%TEMP%\mm_gate4`
(8/8 byte-identical).

| Case | Expected | Measured (`%TEMP%\mm_f1`) |
|---|---|---|
| 19 cases (Task 12 set, mp_marker_uni, mp_setmode_per, mp_setpasses_per, mp_temporal_per, mp_race_per) | `ok`, exit 0, no debug-layer error | all `ok`; debug layer 0 errors in the five lifetime cases |
| mp_setmode_per (Mode -> Off at 80, inside a Peripheral run) | run ends, no core call meets the change, game re-creates, later menus work | `[set] frame 80: setting 0 = 0 (status 0)`; `left 80` with the settings blocker; at frame 90 `layout changed, model re-created at 1920x1080 (1)`; 9 later runs, 9 with their own pass (direct path) |
| mp_setpasses_per (ModelPasses -> 2 at 80) | the same | `left 80`, blocker; at frame 90 `model pass 2 real feature ... created at 1728x972`, `model pass 2 evaluated successfully`; 9 later runs, 9 with their own pass (core, two passes) |
| mp_temporal_per (Peripheral, TemporalMode 1) | blocked in this round, no core call | blocker in the tab, no menu run, no `menu core pass` line (the case was replaced by the temporal cases below) |
| mp_model / mp_model_cl | unchanged | 34.59-35.12 (mean 34.81) / 34.54-35.12; after exit worst -5.68 |
| mp_model_uni / mp_model_per | unchanged | 36.55-37.05 (mean 36.74) / 34.42-36.03 (mean 35.10); after exit -9.84 (Reset's cost, control mp_marker_uni identical) / -5.91 |
| mp_race_per | as before | `ok`; the release waited 1.24 ms for the present under way (the pipeline lock orders it before the feature-call lock; `WithFeatureForMenu`'s `try_to_lock` refusal is not reached by it), then 13.1 ms for the last pass |
| presents that showed the last pass again | recorded | 0 in every case: bench12 waits for its queue after every present, so no pass is ever in flight at the next one. The path is covered by reasoning and awaits the in-game check |

Waits the core can still take inside a menu call, all bounded:
- `Ctx().mutex` is blocking; another thread's feature creation or `SetSettings` can hold it for the length of an NGX
  create.
- The descriptor pools wait up to 100 ms only when every slot of a ring is unpassed. The pack and warp rings have 64
  slots or more, and the temporal ring 256. After rule (2), the unpassed slots are at most the previous pass (its
  recording signal was just enqueued on an idle queue) plus one pending recording per unexecuted private list (a ring
  of 3). So the wait cannot be reached in a menu.
- The temporal and spread GPU waits (`WaitForGpu`) only run with a temporal mode, which rule (3) blocked in this round.

## D3D11 (stage 2, Task 10)

The D3D11 bench `tools/bench/pw_bench.exe` in `tools/bench/run/menu_bridge` (ReShade 6.8, dlss5-dx11-bridge 1.0.20,
renodx-dlss5 v4.7, NR 310.8; see the spike page 0c, "Reproduce"). Build: `feature/menu-mode` with Task 10.
`menu_bridge.ps1 -Out %TEMP%\mm_t10_b -Case mb_off,mb_off_dbg,mb_nr_ref,mb_marker,mb_marker150,mb_marker_dbg,mb_fail,
mb_fail_dbg,mb_model,mb_model_dbg,mb_exit,mb_exit_off,mb_model_per,mb_model_per_dbg,mb_off_per,mb_nr_ref_per`, then
`menu_bridge_compare.py` and `menu_cpu.py` as in the Task 10 brief. The `_per` cases (new) run Mode Peripheral, so the
menu frame goes through the core on the bridge's private queue.

| Criterion | Case | Measured |
|---|---|---|
| all 16 cases | exit 0, `device ok` | all exit 0, every menu case `ok` |
| Marker in the menu, same frame | mb_marker | 10/10 red, 0 pixels differ outside the square; 0/50 after exit (50/50 identical) |
| Entry rule 150 ms | mb_marker150 | 10/10, 0/50, minimum entry 153 ms |
| Forced refusal | mb_fail (+ _dbg) | forced True; 10/10 in menu and 50/50 after exit identical to the bench's frame |
| Model, Mode Off (direct call) | mb_model | enter 10 exit 10, 0 refused, no stop; in menu vs the as-is frame 28.66-41.07 dB (avg 32.09; spike 32.1); vs in-game NR (mb_nr_ref) model 32.81, as-is 30.37 dB; 50/50 after exit identical |
| Model, Mode Peripheral (through the core) | mb_model_per | enter 10 exit 10, 0 refused, no stop; `menu core pass: the first menu frame went through the core (evaluate 0, path 1, warp path 1, model 0x1)`; in menu vs as-is 28.67-40.72 dB (avg 32.01); vs in-game NR Peripheral (mb_nr_ref_per) model 31.95, as-is 29.91 dB; 50/50 after exit identical |
| No core call over an unfinished pass | mb_model_per (+ _dbg) | 37 (44) presents showed the last output again (`reuse` events), none on the direct path |
| Exit inside a menu | mb_exit | run entered at 384, process ends inside it: exit 0, `device ok` (as mb_exit_off) |
| Debug layers | mb_marker_dbg, mb_fail_dbg, mb_model_dbg, mb_model_per_dbg | D3D12: 0 corruption, 0 errors, 0 warnings; D3D11: 4 errors, 1320 warnings in each, the same as mb_off_dbg |
| Watched feature's device line | every case | `private queue on the host list's device ... DIFFERS FROM the NR colour's device` |
| C1: private queue registered | every case | before its warm-up: `the bridge's private queue ... handed to the core under device ...: native case (the host list's device differs from the NR colour's, no queue event during its creation); unregistered once drained` (fix round 1: the wrapped case, where the host list's device is the NR colour's or a queue event arrived during its creation, keeps the queue registered for the session and is not reached on this stand) |
| C2 entry (Task 6 note) | every menu case | ReShade reports the host bridge's D3D12 queue executing the NR list, so the entry is ordered on the GPU: `menu mode: the host runs NR on its own queue ...; menu passes are fenced against it`; no "waiting for the host's NR queue" |
| C2 exit | mb_model / mb_model_per | 10/10 exits `a GPU wait on the host's NR queue` (the last pass still ran) / 10/10 `nothing to order`; no CPU wait, no quarantine, no withheld evaluate |
| CPU per menu present | mb_model | pipeline without the evaluate avg 0.422 ms (p95 0.635), 0.399 after a run's first present; whole present 0.697 ms |
| CPU per menu present | mb_model_per | pipeline without the evaluate avg 0.441 ms (p95 0.683); whole present 0.878 ms (p95 1.448, max 14.5 ms: one core evaluate recording of 13.4 ms) |
| GPU per menu present | mb_model / mb_model_per | private list avg 16.1 / 13.4 ms |

D3D12 after the shared-code change (`OrderRunEntry`, `Quarantine`): bench12 `mp_sigfail,mp_model,mp_model_cl,
mp_model_per,mp_exit_model` all `ok` (10/10 runs, sigfail one stop at 74 with the frame untouched).

Not driven on the D3D11 bench: a feature release or model re-creation inside a menu (the bench never releases its
feature), a resize, the `destroy_swapchain` drain, a Signal/Wait failure (DebugMenuPass=3 is not wired on the bridge and
is shown as "unavailable"), a host whose NR queue ReShade does not report (the entry then stays "waiting for the host's
NR queue" and the exit falls back to the bounded CPU wait), the wrapped-device keying case, a device removal (the
bridge is then kept, never released), a failed warm-up Wait/Signal (a stop for the session), a second D3D11 device's
swap chain (ignored, logged once), and temporal modes (driven in "Stage 3: temporal", on the D3D11 bench too).

Fix round 1 rerun (`%TEMP%\mm_t10_f1`): mb_marker, mb_fail, mb_model (in menu 28.72-41.07 dB, avg 32.11; vs in-game NR
32.83 against as-is 30.37), mb_exit, mb_model_per, mb_model_per_dbg (D3D12 layer 0 errors/warnings) all ok; every log
states the native case; bench12 mp_model ok.

## Stage 3: temporal (Task 13, the user's cadence in menus)

With Interpolate (sync) (`TemporalMode` 1; 2 reads as 1) a menu run goes through the core: the model runs every N-th menu
frame (`TemporalEvery` 4 here) and the frames between are carried along NVIDIA optical flow, because menus have no game
motion. The core's motion source goes to optical flow at a run's first core frame and back to the game's vectors at the
run's end, before the game's next evaluate. Mode 3 is never used in menus. Without optical flow nothing is carried.
Builds: `feature/menu-mode` `1405762` (Task 13), fix round `09395be`, core optical-flow fence fix `d6f9923`. bench12 with
`--nr-release-thread`; static camera, 10 menus; outputs under a local bench folder. D3D11: `menu_bridge.ps1`.

```powershell
powershell -NoProfile -File tools/bench12/reference_dumps.ps1 -Runtime run_nrhost -Out $env:TEMP\mm_t13 -Case 'mp_model_t1,mp_ref_t1,mp_off_t1,mp_model_per_t1,mp_ref_per_t1,mp_off_per_t1,mp_marker_t1,mp_marker_per_t1,mp_teardown_per,mp_teardown_per_t1,mp_noflow_t1,mp_noflow_per_t1,mp_bg_per,mp_model_t3,mp_model_per_t1_dbg'
python tools/bench12/menu_psnr.py $env:TEMP\mm_t13 mp_model_per_t1 mp_ref_per_t1 --off mp_off_per_t1
powershell -NoProfile -File tools/bench12/reference_dumps.ps1 -Runtime run_nrhost -Out $env:TEMP\mm_t13_f -Case 'mp_flowfail_t1,mp_flowfail_per_t1,mp_flowlast_t1,mp_flowlast_per_t1'
powershell -NoProfile -File tools/bench/menu_bridge.ps1 -Out $env:TEMP\mm_t13_d11 -Case 'mb_model,mb_model_per,mb_model_t1,mb_model_per_t1,mb_nr_ref_t1,mb_nr_ref_per_t1'
```

### Cadence and quality (D3D12)

Each case is compared with its own every-frame reference (the in-game NR run with the same temporal setting). The in-menu
dump is always the 10th menu present, one frame after a model frame, so only such age-1 carried frames are measured.

| Case | Expected | Measured |
|---|---|---|
| all 22 bench12 cases (Task 13 set and the Task 12 regressions) | `ok`, exit 0 | all `ok`; every model case enter 10 exit 10, no stop, 0 refused or skipped presents |
| release gate (MenuMode=0, `-WarpPath pixel`, off_t0/warp_t0/warp_t1/uni_black) | 8 dumps byte-identical | 8/8 identical, after Task 13 and after its fix round |
| mp_model_t1 (Off): cadence per run | model every 4th frame | 24 frames through the core, 6 with the model, 18 carried with the flow field (one run 25/7/18); model on frames 1, 5, 9, ... |
| mp_model_per_t1 (Peripheral): cadence per run | the same | the same pattern; in-menu dumps 10/10 carried with the flow field |
| mp_model_t1 in menu vs mp_ref_t1 | above the pause without menu mode | min / mean **32.96 / 33.40 dB**; pause without menu mode 29.25 |
| mp_model_per_t1 in menu vs mp_ref_per_t1 | same | **33.19 / 33.88 dB**; pause without menu mode 28.21 |
| carried frame vs every-frame reference, at the dump indices the every-frame case shares | close to the every-frame menu | Off 34.30, 34.61, 35.12 against 34.65, 35.01, 34.97; Peripheral 36.24 ... 34.28 against 36.13 ... 34.29: within -0.4 / +0.2 dB |
| carried frame vs the pause without menu mode | above | +4.2 dB (Off), +5.7 dB (Peripheral) |
| mp_model (Off, every frame) and mp_model_per, unchanged by Task 13 | as the layout section | 34.51 / 34.86 and 34.25 / 35.11 (min / mean, fix round) |
| after exit, worst of the first 5 frames vs pause | recorded (Task 12 bound -6.7 dB) | **-7.04 dB (Off), -7.17 dB (Peripheral)**; every frame: -5.68 / -5.91 |
| mp_marker_t1, mp_marker_per_t1 (control: marker in menus, no model, same exit Reset) | - | -7.04 / -7.17 dB: identical to the model cases |
| GPU per menu present | recorded | sync cadence 2.28-2.46 ms average against about 5 ms every frame |
| CPU per menu present (whole present) | recorded | Off every frame 0.72 ms; sync 1.12 ms average including one 59 ms first frame, about 0.87 ms without it |

The after-exit number is set by the exit Reset plus the cadence restarting after the motion source goes back to the game's
vectors, not by the carried menu frames: the marker controls, with no model in menus, give the same frames. It is past
Task 12's -6.7 dB bound, which was set for every-frame menus. Controller ruling 2026-09-29: accepted like Uniform (bound
relaxed; Limits: up to about 7 dB in the first frames); listed under the in-game checks.

The first menu frame of a process with the sync mode takes about 59-60 ms: the core creates its optical flow session inside
the first core evaluate. The session lives for the process, so later menus do not pay it.

### Lifetime, no optical flow, other modes

| Case | Expected | Measured |
|---|---|---|
| mp_teardown_per_t1 (C5: the swap chain recreated inside a Peripheral sync run, `--recreate-swapchain 75`) | flow off before the game's next evaluate; a new run on the new swap chain; 9 later runs with their own pass | events in order: enter 67, `cadence 8 2 6`, `drain 75`, `flow off 75`, `enter 75`, `exit 90`, `flow off 90`, `game 90 vectors`; 9 later runs; no stop; no `flow owed` or `withheld` line; ok |
| mp_teardown_per (control, TemporalMode 0) | the same without the flow | ok, 9 debug messages |
| mp_noflow_t1 (C6, Off, `DebugMenuNoFlow=1`) | the direct pass on every menu frame | 10/10 runs direct, no `core` or `cadence` event; one log line `optical flow is not available (...)` |
| mp_noflow_per_t1 (C6, Peripheral) | unavailable, no run | `Menu mode: unavailable: optical flow is not available (forced by DebugMenuNoFlow=1)`, no menu run |
| mp_bg_per (Peripheral, mode 3) | unavailable, no core call | `unavailable: the background temporal mode is not used in menus: use Every frame or Interpolate (sync) (retried automatically)`, no `menu core pass` line |
| mp_model_t3 (Off, mode 3) | the direct pass | 10/10 runs direct, no `menu core pass` line |
| mp_flowfail_t1, mp_flowfail_per_t1 (`DebugMenuNoFlow=2`: one flow failure on the first run's 10th core frame, a carried one) | that frame runs again as a full frame, then nothing is carried | `redo 1`, the in-menu dump is that frame (34.96 dB Off against 34.94 every frame; 36.12 Peripheral against 36.25); Off then runs the direct pass in later runs, Peripheral's run ends `left` and no run enters again |
| mp_flowlast_t1, mp_flowlast_per_t1 (`=3`: the same, and the full frame fails too) | the last output is shown again | `redo 0`; the dump is the previous model output, one frame old: 33.31 dB (Off), 35.10 dB (Peripheral) |
| mp_race_per, mp_setmode_per (regression) | as in the layout section | an event inside a run, 9 later runs with their own pass |
| debug layer (mp_model_per_t1_dbg) | 0 errors | 0 errors, 0 corruption |

A real optical-flow `Execute` failure and a core that refuses the switch back were driven only through `DebugMenuNoFlow`
and a fake switch in `tests/test_menu_flow_switch.cpp` (the bench core never refuses, and the bench GPU has a working
engine).

### D3D11 through the bridge

`mm_t13_d11`, and `mm_t13_f1d11` after the fix round (dlss5-dx11-bridge, as in the D3D11 section). Model against in-game NR,
average dB (the as-is frame in brackets):

| Case | Measured | Every frame (Task 10) |
|---|---|---|
| mb_model_t1 (Off, sync) | 33.19-33.23 (30.71) | mb_model 32.81 (30.37) |
| mb_model_per_t1 (Peripheral, sync) | 32.34-32.35 (30.51) | mb_model_per 32.08 (29.93) |

Both: 10/10 runs, 50/50 after-exit frames identical to the bench's frame, cadence lines of 17-19 frames with 5 with the
model, in-menu dumps 10/10 carried with the flow field.

### Optical flow ordering (core fix `d6f9923`)

Two items from Task 13 are closed by the optical flow investigation:
- The `ID3D12CommandQueue1::Wait ... fence value of zero` debug-layer warnings (3 per process) were the core's own: it
  registered its textures with fence value 0.
- The intermittent debug-layer failure of `mp_teardown_per_t1` (about 1 run in 3: `OptimizerFps flow: this frame` /
  `residual frame` still in flight on another queue) was a real missing order, not a debug-layer limit: nothing ordered the
  first list that writes the flow images behind NVIDIA's registrations. It needs no menu: a test with no host at all
  failed 5 of 5 before the fix. The fix waits on the CPU once for the registrations (1 s cap, inside the session creation
  that already costs about 50-60 ms) and gives every registration and execute a non-zero fence ticket.

| Check | Before | After |
|---|---|---|
| `tests/test_flow_session.cpp` (ctest `ofps_flow_session`; needs an NVIDIA GPU and the debug layer, else skipped) | 5/5 fail | 10/10 pass, no warnings |
| mp_teardown_per_t1 | 3 of 6 fail (2 of 8 on `1405762`) | 12/12 ok; each run 9 debug messages, all the usual `CreateCommittedResource` warnings; 0 errors, 0 value-0 warnings |
| mp_model_per_t1_dbg, mp_model | ok | 3/3 and 1/1 ok |
| game frames through the core with flow (bench12 `--host core --core-flow`) | 0 errors in 8 of 8 | 0 errors, 0 warnings in 4 of 4 |

## Final review fixes

| Check | Before | After |
|---|---|---|
| mp_reenable (menu mode off, model re-created, on again inside an open menu; debug layer) | a run entered at 130 on the old model's snapshot; NGX logged `EvaluateFeatureCommon: Error: invalid handle` | no run in that menu, 8 later runs with their passes, 0 debug-layer errors |
| mp_model, mp_model_per, mp_model_cl, mp_race_per, mp_recreate, mp_resize, mp_model_per_t1_dbg, mp_sigfail | ok | ok (enter/exit and checks unchanged) |
| mp_teardown_per_t1 (the drain waits without the pipeline's lock, one 500 ms deadline) | ok | 3/3 ok, 0 debug-layer errors |
| mb_model, mb_model_per, mb_exit (D3D11) | ok | ok; no `menu bridge debug` line without `DebugMenuBridgeCanary=1` |
| Release gate, MenuMode=0 (`-WarpPath pixel`) | 8/8 | 8/8 byte-identical |

## In-game checks still owed (owner)

Status 2026-10-03: menu mode works in Star Wars Jedi: Fallen Order (OptiScaler's D3D11 path, Luma's scRGB HDR) after
the `ExecuteCommandLists` queue hook and the scRGB fix; the menus of Baldur's Gate 3 are not checked yet. The list
below is kept as it was written before that check.

From `docs/dev/menu-mode-spikes.md`, "In-game checks"; 3 and 9 are the main risks.
1. Native D3D12 game: the log line with the host list's device and whether it equals the NR colour's device.
2. Fallen Order (pause, map, workbench) and BG3 menus: NR look, no flash in or out, readable text, no hang or black frame.
3. **Main risk. Moving menu backgrounds:** menus have no motion vectors, so NR history smears moving content
   (`mp_model_motion` above). If it shows, a history reset while the background moves is the simple fix.
4. Ghosting in the first frames after a menu (bench with the exit Reset: -5.68 dB worst over the first 5; -7.04 dB / -7.17 dB with the sync cadence).
   With the sync cadence also: the single 60 ms hitch at a session's first menu, and carried frames on a moving menu background.
5. GPU cost in menus at the game's resolution, and whether the budget cap holds.
6. HDR10 and scRGB games: HDR10 suspends menu mode without harm; scRGB shows the right white scale.
7. OptiScaler F6 does not switch NR off inside menus while menu mode is on (accepted cost).
8. Resize, fullscreen toggle and quitting inside a menu (the bench covers a same-size `ResizeBuffers` and a resolution
   change that re-creates the feature, see C7).
9. **Main risk. AIO and Feeder hosts** (compute list, other queues): the menu pass and the snapshots are unverified in
   real games (the bench covers a compute-list host only).
10. D3D11 games (Fallen Order, BG3): the C1 log line says "native case" or "wrapped case"; quitting or a feature
    release while a menu pass runs (`the watched feature is released with the bridge's last pass at b ...`), no hang.
11. D3D11 bridges other than dlss5-dx11-bridge: a host bridge that makes its D3D12 queue wait for a D3D11 value it
    signals only after our `Wait(b)` would deadlock the three queues (the entry fence's residual, as Task 6's shared
    compute queue); watch for a TDR at a menu's entry.
12. A GPU slower than 100 ms per menu pass (007 First Light when VRAM runs short): at a menu exit the log says
    `a menu pass still ran 100 ms into the game's NR evaluate ... menu mode stays on`, a few game frames go without NR,
    then `the outstanding menu pass completed ... run again`; the next menu gets passes again.

## Paths no bench drives

- A full ring of in-flight menu passes.
- The `destroy_swapchain` drain timeout (500 ms in all, waited without the pipeline's lock) and the late release after
  it; a second swap chain (ignored while menu mode serves the first one, final review).
- A device removal after menu mode ran (the graveyard, the drain and the proofs keep everything; unit tests only).
- A menu pass slower than 100 ms at an exit (held as outstanding, evaluates withheld without waiting; unit tests only)
  and a core descriptor pool with no free slot in a menu call (the core's fallback is recorded, not shown; unit test of
  the pool only).
- A real device-level `Signal`/`Wait` failure (only the forced one, `DebugMenuPass=3`, `mp_sigfail`).
- A swap-chain size change while the watched feature stays alive (the bench's resolution change always re-creates
  feature 18, so the pipeline's in-run `resize` end and rebuild are reached only through the release path).
- Stage 3: a model creation inside a menu call despite the settings rule (a profile change, a failure path). The
  creation is refused and a rebuild is requested (`menu core pass: the core needed a new model inside a menu ...`).
  The core scenario test `ScenarioMenuRebuild` covers the core's side; the pipeline's side is covered by reasoning.
- Stage 3 temporal: a real optical-flow failure (`Execute`, session or fence) and a core that refuses the motion-source
  switch back; only the forced key and the fake switch drive them. Carried frames 2-3 after a model frame (only age-1
  frames are dumped). A second NR feature evaluating during a menu run uses the menu's optical flow (it does not end the run).
- Stage 3 on D3D12: a present while the previous menu pass still runs (the last output shown again, see fix round 1);
  bench12 waits for its queue after every present. The D3D11 bench drives the same rule on the bridge (mb_model_per,
  37 presents).
