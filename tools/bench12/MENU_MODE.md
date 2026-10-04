# bench12: menu-mode options and cases

Menu mode (the add-on runs DLSS Neural Rendering on the presented frame while the game stops calling it) is measured
on bench12 with the options below and the `mp_*` / `pp_*` cases of `reference_dumps.ps1`. Results:
`docs/dev/menu-mode-acceptance.md`. The main option table is in [README.md](README.md).

## Options

| Option | Default | Meaning |
|---|---|---|
| `--nr-pause A,B[,A2,B2...]` | none | Frames A..B-1 skip the feature-18 evaluate (a "menu"; needs `--nr native`). |
| `--nr-pause-show frozen\|input` | `frozen` | What a paused frame presents: the last NR output as it stands, or the frame's own NR input (the sRGB proxy decoded, or with `--nr-colour linear` the linear colour as it is: a menu drawn without NR). |
| `--nr-recreate F` | none | Frame F releases feature 18 (after the bench's queue is idle) and creates it again before its evaluate; inside an `--nr-pause` range the new feature is first evaluated when the pause ends. Logs `[nr] frame F: feature 18 released`. Needs `--nr native --host ngx`. |
| `--nr-release-thread` | off | With `--nr-recreate F`: the release runs on a second thread that starts during frame F's present (where the add-on runs its menu pass), so it races the menu call (plan correction C4); the bench neither evaluates nor creates feature 18 until the thread returned, then creates it again. Logs `[nr] frame F: feature 18 released on a second thread`. |
| `--set-at F,ID,VALUE[,F2,ID2,VALUE2...]` | none | Frame F sets add-on setting `ID` (`OfpsSettingId`) to the integer `VALUE` through the add-on's `OptimizerFpsSetSettingV1` export, as the overlay would inside a menu; more triples, more changes. Logs `[set] frame F: setting ID = VALUE (status S)` (0: applied). |
| `--resize-at F[,W,H]` | none | Frame F resizes the swap chain: `ResizeBuffers` at the same size (a fullscreen toggle), or to WxH (a resolution change: the renderer, DLSS and feature 18 are rebuilt at the new size). Logs `[info] frame F: swap chain resized`. Not with `--interactive`. |
| `--recreate-swapchain F` | none | Frame F waits for the queue, releases the swap chain (ReShade's `destroy_swapchain`, not a resize: menu mode drains) and creates a new one with the same description on the same window. Logs `[info] frame F: swap chain recreated`. Not with `--interactive`. |

## Cases and checks (`run_nrhost`, opt-in: name them with `-Case`)

- `mp_marker`, `mp_fail`, `mp_sigfail`, `mp_exit`: the pipeline's diagnostics (`DebugMenuPass` 1, 2, 3; exit inside a menu).
- `mp_model*` with `mp_ref*` (every-frame NR) and `mp_off*` (the pause without menu mode): `menu_psnr.py`, `menu_cpu.py`.
- `mp_recreate`, `mp_resize`, `mp_resize_size` (plan correction C7): the event at frame 80, inside the first pause while
  its menu run is under way, debug layer on. `menu_check.ps1` expectation `lifetime` (`Test-MenuLifetime`) requires the
  bench's line for the event at that frame and the add-on's view of it (`release 80` in `menu_events.log`, or ReShade's
  `ResizeBuffers` line), a menu run entered before frame 80 and ending at or after it, and every run entered later with
  its own recorded pass (an in-menu dump at a present whose `t` line has a nonzero evaluate recording). The same case
  without its flag fails the check.
- `mp_model_uni`, `mp_model_per` with `mp_ref_uni`/`mp_ref_per` and `mp_off_uni`/`mp_off_per` (stage 3, Task 12): the
  menu frame through the core with Mode Uniform / Peripheral; `menu_psnr.py` as for `mp_model`.
- `mp_race_per` (C4): as `mp_recreate` with Mode Peripheral and `--nr-release-thread`; expectation `lifetime`, event
  `race` (the add-on's `release 80` or `release 81`: the release lands during present 80 or just before it).
- `mp_setmode_per` (Mode -> Off, id 0) and `mp_setpasses_per` (ModelPasses -> 2, id 20) (Task 12 fix round 1): a
  setting changed at frame 80 inside a Peripheral run; expectation `lifetime`, event `setting` (the bench's `[set]` line
  with status 0, the add-on's `left 80`/`left 81` and its `a setting changed in this menu` status line), then 9 later
  runs with their own pass (the game's evaluate at frame 90 applied the change).
- `mp_reenable` (final review C1): MenuMode off at frame 92, the model re-created while it is off (Mode Off -> Uniform
  at 94 -> Off at 97), MenuMode on again at 130 inside the open menu 120-150, debug layer on. Expectation `reenable`
  (`Test-MenuReenable`): the bench's `[set] frame 130: setting 45 = 1` line, no run entered from 130 for 30 presents
  (the old model's snapshot is never used; before the fix a run entered at 130 and NGX logged `invalid handle`), and 8
  later runs, each with its own pass.
- `mp_model_t1`, `mp_model_per_t1` with `mp_ref_t1`/`mp_ref_per_t1` and `mp_off_t1`/`mp_off_per_t1` (stage 3, Task 13):
  TemporalMode 1 (sync, TemporalEvery 4) in menus, Mode Off and Peripheral: the model every 4th menu frame, the frames
  between carried along the optical flow. `menu_events.log` has a `core <model> <flow field>` line per frame through the
  core (before that present's `t` line) and one `cadence <frames> <with the model> <with the flow field>` line per run
  (the ReShade log the same as a sentence); `menu_psnr.py` as for `mp_model`.
- `mp_teardown_per_t1` (C5): `--recreate-swapchain 75` inside a Peripheral sync-cadence run; expectation `lifetime`,
  event `teardown` (the bench's `swap chain recreated` line, the add-on's `drain 75` followed by `flow off 75`, and the
  next host evaluate's `game <present> vectors`: the core had the game's motion vectors again when the game's next
  evaluate reached it), then 9 later runs with their own pass. Every menu check also fails on a `flow owed` or
  `game <present> withheld` line (a switch-back the core refused). Before the core fix `d6f9923` the debug layer failed this
  case about 1 run in 3 (flow textures registered with NVOFA without a fence); since the fix it passes 12 of 12. `mp_teardown_per`: the same at
  TemporalMode 0 (control). `mp_model_per_t1_dbg`: `mp_model_per_t1` with the debug layer. `mp_marker_t1`,
  `mp_marker_per_t1`: the marker in sync-cadence menus, the control for the after-exit frames (exit Reset and the
  cadence's restart without any model pass in menus).
- `mp_noflow_t1`, `mp_noflow_per_t1` (C6): TemporalMode 1 with `DebugMenuNoFlow=1` (menus act as if the core had no
  optical flow). Mode Off: expectation `model`, the direct pass on every menu frame (no `cadence` line, no `menu core
  pass` line). Peripheral: no menu check, the log must show `unavailable: optical flow is not available`.
- `mp_flowfail_t1`, `mp_flowfail_per_t1`, `mp_flowlast_t1`, `mp_flowlast_per_t1`: the optical flow fails once, on the
  first run's in-menu dump frame (a carried one, `DebugMenuNoFlow=2` / `3`). Expectation `flowfail`: one `redo 1` (the
  frame ran again as a full model frame in the same list) or `redo 0` (the last output shown again), the in-menu dump is
  that frame, then no core frame: Mode Off runs the direct pass (10 runs), Peripheral's run ends `left` on the next
  present and no run enters again.
- `mp_bg_per`: Peripheral with TemporalMode 3; no menu check, the log must show `unavailable: the background temporal
  mode is not used in menus` and no `menu core pass` line. `mp_model_t3`: Mode Off with TemporalMode 3 runs the direct
  pass on every menu frame (no `menu core pass` line).
- `mp_marker_uni`: the marker in Uniform menus, the control for the exit Reset's after-exit cost.
- `pp_ref`, `pp_trace`, `pp_own` (also on `run_r521`, `run_addon`): the host's own evaluates with menu mode tracing and
  with its own parameter block; `pair_psnr.py` against `pp_ref`.

```powershell
powershell -NoProfile -File tools/bench12/reference_dumps.ps1 -Runtime run_nrhost -Out $env:TEMP\mm_life -Case 'mp_recreate,mp_resize,mp_resize_size,mp_ref'
```
