# Camera-motion carry, step 0: the attribution harness

Step 0 of the camera-motion carry work: what the loss of the carried frames
(TemporalMode 1, N = 8) under camera motion is made of. The scripts need Python 3.11 with numpy and Pillow; they
reuse `../foliage/` (`f16io`, `foliage_metrics`, `ghost_metrics`, `repeats`, `run_foliage`). The bench folder
(`<root>`: runtimes, `out\`, `bench.lock`) and the scene folder come from `tools\bench12\bench_paths.json` (not tracked;
copy `bench_paths.example.json`); the metric scripts also take `--root <root>\out\motion`.

| File | Role |
|---|---|
| `run_motion.py` | Stages `<root>\rt_motion` / `rt_motion_bg` and runs one cell under `bench.lock`. |
| `run_variants.py` | Several short runs of one view in one bench process (`--variants`), one cell per run ("Variants"). |
| `motion_ofac.py` | The cadence phase P from R0's OFAC dumps; the check of every run's OFAC evals against R0's and of the dump window's continuity. |
| `motion_metrics.py` | M1, M2, the off-screen split, chain statistics, the d check, mode 3, repeats → `<view>\metrics.json`. |
| `motion_band.py` | The raw fields of a view: true offset, off-screen mask, the disocclusion band and its union per cycle. |
| `motion_report.py` | `out\motion\attribution.md` from every view's `metrics.json`. |
| `motion_sheet.py` | A PNG sheet of a view (needs its `metrics.json`): offsets 1 / 4 / 7 × four runs (default T0 / R0 / R1 / R2, `--cols`) and error heat maps → `out\motion\sheets\`. |
| `motion_maps.py` | Step 1 maps: the per-pixel M2 flicker map on foliage (the whole frame on a view without a vegmask), R0 against a run; crops of the ghost band, R0 against a run; step 1c: where a `<run>_raw` (OFAC v3) restarts the model's and the carry chain, the floor outlined, with the restart shares → `out\motion\sheets_step1\` (`--out`). |
| `motion_split.py` | Step 1b: M2 by region (band, frame border, depth edges, floor, near, rest; from R1_raw's depth and the prevdepth band) and by offset, R0 against runs, and each region's contribution to the whole-frame M2 → `<view>\split.json`. |
| `chain_cost.py` | Step 1 cost: R0 and C1-C4 (or S1-S6) at 3840x2160 interleaved under `bench.lock` in `rt_motion`, three rounds (`../foliage/rule_cost.py`'s sampling, `DebugTiming`) → `out\motion\cost_combo\summary.json` (S: `--name cost_combo_step1b`). Chain = Accumulate + Refine + LinkFail and the merged AccumulatePair / RefinePair / LinkFailPair (step 1c; step 1d has no LinkFailPair) per frame (over Expect's count); Task 2's C numbers were read without Refine. |
| `short_compare.py` | A short run's dumps against the full run's: byte identity, PSNR per frame, per offset and over the window's first 8 frames ("Short runs"). |
| `test_motion_metrics.py` | Synthetic checks; `python -m pytest tools/bench12/motion -q`, or without pytest `python tools\bench12\motion\test_motion_metrics.py` → `ok`. |

## Commands

```
python tools\bench12\motion\run_motion.py --stage              (rt_motion: this checkout's bench, add-on, core, forwarder, shaders)
python tools\bench12\motion\run_motion.py --stage --mode 3     (rt_motion_bg: a copy of rt_foliage_bg, the RenoDX consumer)
python tools\bench12\motion\run_motion.py --view <view> --run <run> [--mode 1|3] [--repeat k] [--raw] [--frames F] [--first S] [--out DIR]
                                  [--exposure record|replay] [--short [W]] [--nr-reset [W]] [--runtime DIR]     (mode 1; "Short runs")
                                  [--bench-arg ARG ...] [--no-compact]
python tools\bench12\motion\run_variants.py --view <view> --runs T0,R0[,...] [--short W] [--out DIR] [--runtime DIR] [--no-compact]
python tools\bench12\motion\short_compare.py <full cell> <short cell> [--phase P] [--json FILE]
python tools\bench12\motion\motion_metrics.py <view> [--root DIR] [--mask DIR]
python tools\bench12\motion\motion_report.py [--root DIR] [--out FILE]
python tools\bench12\motion\motion_sheet.py <view> [--root DIR] [--out DIR] [--cycle C] [--gain G] [--cols T0,R0,C2,C1]
python tools\bench12\motion\motion_maps.py flicker|band|restart <view> <run> [--root DIR] [--out DIR] [--cycle C]
python tools\bench12\motion\motion_split.py <view> <run> [<run> ...] [--root DIR]
python tools\bench12\motion\chain_cost.py [R0 C1 ...] [--view combo] [--rounds 3] [--frames 600] [--name DIR]
```

- Outputs: `<root>\out\motion\<view>\<run>[_m3][_raw][_r<k>]\` with `dump_*.png`,
  `ReShade.ini`, `ReShade.log`, the bench's stdout/stderr, `meta.json` (argv, ini, hashes, exit, P, the OFAC check)
  and, with `--raw`, `motion_*.f32`, `depth_*.f32` and the OFAC window. A previous folder of the cell and stale
  outputs in the runtime move to `out\motion\old\<timestamp>\`; nothing is deleted. Staging records the hashes in
  `out\motion\stage_<runtime>.json`.
- Order: R0 of a view first. R1, R2 and R3a take `--cadence-phase P` from R0's full-pass dumps, and every run that
  dumps OFAC is checked against R0 after it ends: the full-pass evals must be R0's (without `--raw`), or the core's
  full-pass index must step exactly after the evals ≡ P (with `--raw`). A mismatch exits 1 with `ABORT` in
  `meta.json["ofac_check"]`; `motion_metrics.py` repeats the check and stops on it.
- A run refuses to start with less than 60 GB free on the bench folder's drive (a raw combo run writes several GB). After it, the cell's
  `*.f32` and `*.bin` are compressed in place (`compact /c /exe:xpress16k`, lossless, one log line; OFAC files 2.8:1);
  `--no-compact` leaves them. Every run passes `--texture-cache <root>\cache\textures` ("Start").
- The bench places its window on the second monitor without focus; a timeout kills only the PID the script started.

## Runs (N = 8; ini section `[OptimizerFPS]`)

| Run | Bench | Ini on top of `run_foliage.BASE` (`TemporalModelMarker` removed: the shipped default 2; `TemporalChainRule=0` pinned: classic) |
|---|---|---|
| T0 | `--nr native` | TemporalMode=0 |
| R0 | `--nr native` | TemporalMode=1, TemporalEvery=8, DebugTemporalDumpAcc=1 |
| R0x | as R0 | R0 + DebugTemporalNoExpect=1 |
| R1 | R0 + `--motion-span cadence --cadence 8 --cadence-phase P` | R0 + DebugTemporalCarryProbe=1 (oracle chain) |
| R2 | as R1 | DebugTemporalCarryProbe=3 (+ accept every in-frame pixel) |
| R3a | as R1 | DebugTemporalCarryProbe=7 (R2 + bilinear residual) |
| R3b | as R0 | DebugTemporalCarryProbe=4 (R0 + bilinear residual) |
| mask | `--nr off --view vegmask` | none (raster garden views only: the regions) |
| prevdepth | `--nr off` + R1's cadence motion + `--dump-raw prevdepth` | none (the exact disocclusion band; any view) |
| C1-C4 | as R0 | R0 + DebugTemporalChainRule=1..4 (step 1: 1 never restart, 2 colour gate only with a depth miss, 3 = 2 + 5x5 vote, 4 depth only) |
| S1-S4 | as R0 | R0 + DebugTemporalCarryChainRule=1..4 (step 1b: the carried frames reproject along a carry chain by that rule; the model's chain stays rule 0) |
| S5, S6 | as R0 | R0 + DebugTemporalCarryChainRule=5, 6 (step 1c: 5 = a colour fail restarts only when coherent, ≥ 18 of 5x5; 6 = 5 + a coherent Refine shift). From core d089024, `--raw` writes OFAC v3 with the carry chain |
| D1 | as R0 | R0 without `TemporalChainRule` (task 3: the shipped default 1 = carry chain rule 5, model rule 0); must equal S5 byte for byte, in mode 3 too |

- `--raw` adds `--dump-raw motion,depth` and `DebugTemporalDumpAccFrom=<first>`: the core dumps the chain on every
  evaluate of `[first, first + 63]`. The dumps then start 8 frames before `first`, because the band needs the depth of
  the full pass up to N frames back. The colour dumps are byte-identical to the run without `--raw` (checked on combo).
- `--mode 3` runs T0, R0, D1, C1-C4 and S1-S6 only, on `rt_motion_bg`, with `--upscaler sr` on the garden views (RenoDX needs DLSS):
  `TemporalMode=3` (or 0), `TemporalEvery=8`, no oracle and no OFAC (both need TemporalMode 1). Three repeats of R0.
- The probe works on the synchronous single-pass path only: the spread path (`ModelPasses` > 1) silently ignores
  `DebugTemporalCarryProbe`. The runs keep `ModelPasses=1`.
- The "acceptance" lines of `ReShade.log` come from the stats pass, which never gets the accept-all flag: under R2 and
  R3a they show the ordinary decision. Judge R2 by the frames only.
- Every key of `run_motion.UNSET` that a run does not set is removed from the ini before it, and all of them after it.

## Views

| View | Bench (phase 1/2 flags, `run_foliage.VIEWS`) | Frames, dumps |
|---|---|---|
| `combo` | garden, `--camera combo --wind` | 1080, 952–1079 |
| `pan` | garden, snap turn of 30° over 176 frames | 176, 48–175 |
| `held` | garden, static camera (control) | 176, 48–175 |
| `lab_combo` | lab path traced, `--upscaler rr --render-scale 0.5`, `--camera combo` | 176, 48–175 |
| `lab_forward` | lab path traced, `--camera forward --move-speed 7` (temporal_quality.py's forward) | 176, 48–175 |
| `ghost_door` | phase 2's `lab_ghost_door`, static camera | 176, 48–175 |
| `lab_combo_raster`, `lab_forward_raster` | the two lab cameras on `run_foliage.LAB_RASTER` (raster, no upscaler) | 176, 48–175 |
| `lab_forward_raster_matte` | step 1c: `lab_forward_raster` with `--gltf out\motion\matte\lab_scene_matte_floor.glb`, a copy of `lab_scene.glb` whose floor material `lab_tex_shell_floor` has roughness 1.0 instead of 0.12 (the bench has no roughness flag) | 176, 48–175 |

Path tracing hands the bench's motion guide to DLSS RR as well, so under `--motion-span cadence` (R1, R2, R3a) RR
reprojects with the offset to the last full pass and the colour the core gets is no longer T0's: R1 / R2 on
`lab_combo`, `lab_forward` and `ghost_door` are invalid for the attribution (T0, R0, R0x and the raw/prevdepth band
stay valid). The raster lab views carry the lab's causes.

`--frames` / `--first` shorten a smoke run (`--frames 200 --first 150`).

## Metrics (`metrics.json`)

- **Phase and offsets.** P = the full-pass eval mod 8 (frame = eval, d = 0 as in phase 1); offset = (frame − P) mod 8.
  Frames before the second full pass (P + 8) and partial cycles at the ends of the dump window are left out
  (`full_cycles`): every offset is measured on the same cycles.
- **`d_check`:** d = 0 is checked on R0_raw, whose chain the core builds itself: on offsets 2–3, where few chains
  restart, its median error against the true offset (rest, else whole) must stay below 0.25 px; with a wrong d the true
  offset would belong to another frame. The check is `valid` only where the median |g| there is at least 2 px (a static
  view passes any d). R1_raw's chain equals its motion dump by construction (the oracle takes it as
  the chain), so its 1 / 0 / 0 checks only the plumbing (scale, sign), not d.
- **`ofac[run]`:** the evals dumped and the ones missing. The window has one readback buffer: an evaluate is lost when
  the CPU runs ahead of the GPU, and the last evaluate of a run is never written (its copy is collected on the next
  one). The chain statistics use the frames present.
- **`m1[run][region][offset]`:** PSNR (from the mean MSE) and MAD against T0, for R0, R0x, R1, R2, R3a, R3b. Regions:
  `foliage_metrics.regions` of the vegmask (the `mask` run, else phase 1's `out\foliage\<view>\mask\r1`), `whole`, and
  `whole_offband` where R1_raw is present, and its complement `band` (the cycle's union: the ghost guard's region).
- **Disocclusion band** (`motion_band.py`), span = offset (8 on a full pass): p is in the band if its source
  q = floor(p + 0.5 + g), g = R1_raw's motion (the true offset to the last full pass), is off screen, or if
  |z_prev(p) − z_full(q)| / max > 5 %: z_prev(p) = the `prevdepth` run's depth of p's surface in the full pass's camera,
  z_full = R1_raw's depth of the full-pass frame, both view depths in metres. Without the prevdepth run the band falls
  back to phase 2's rule (this frame's own depth, not moved to the full pass's camera), which grows with camera motion
  (combo smoke: 1 % of the frame at offset 1, 35 % at offset 7, 42 % on the full pass, against 1 %, 4.7 % and 5.3 % with
  prevdepth); `band_rule` then reads `approximate` and every off-band number is an estimate.
- **`whole_offband`:** the complement of the union of the bands of the frame's cycle (frames P + 8c … P + 8c + 7), so
  offset 0 and offsets 1–7 are measured on the same pixels. R2 and R3a (accept-all) are read there.
  `band_share[offset]` / `band_union_share[offset]`: the frame's share in its own band and in the cycle's union.
- **`offscreen[run]`:** on carried frames, the pixels whose source p + g lies off screen against the rest: pixel share,
  share of the squared error, PSNR inside and outside.
- **`m2[run][region]`:** flicker, the mean |e_t − e_t−1| of the luma error against T0 (and of its 16 px tile average).
- **`chain[run][region][offset]`** (R0_raw, R1_raw; carried frames in the window; motion-texture px): coverage =
  median |a| / |g| over |g| > 2 px; median and p90 of |a − g|; restart share = |a − mv_t| < 0.05 px among the pixels
  with |g − mv_t| > 1 px, offsets > 1 (mv_t = the run's own motion dump). Per-frame values are averaged per offset.
  R1_raw's restart share is always NaN ("-"): its own motion dump is the true offset, so no pixel has |g − mv_t| > 1.
- **`mode3`:** PSNR of each R0_m3 repeat against T0_m3 over all dumped frames, the mean and the spread (max − min).
- **`repeats`:** byte identity of `<run>_r<k>` against `<run>` (PNG and OFAC files).

## Report

`motion_report.py`:
- per view, for every run: offset 0, the offsets 1–7 mean, offset 7, the drop from offset 0 (1–7 mean − 0 and 7 − 0),
  the same off-band, and M2;
- the dB per cause (cause 1 = R1 − R0x, cause 3 = R2 − R1, cause 2 = R3a − R2 and R3b − R0, expectation = R0 − R0x),
  once per base: whole frame, off-band, and the drop from offset 0. The oracle also gives the model the exact motion on
  the full frame, so R1 raises offset 0 too; the drop base attributes the carry alone;
- the floor: R2's carried frames against T0 and against its own offset 0, on one region each (whole; the off-band mask);
- the band share by offset; the off-screen share; the chain statistics; M2; mode 3; the checks (OFAC gaps, d check,
  repeats);
- step 1, when C1-C4 are present: criteria 1-5 of the plan's bar per candidate against R0 (pass/FAIL with the numbers;
  the cost from `cost_combo\summary.json`, the chain passes' delta per frame), the winner (passes 1-5, the largest
  foliage M2 gain on combo + pan, ties to the higher whole 1-7), the deltas by view and the foliage chain statistics of
  `C<n>_raw`. A run or view that is missing fails its criterion. Criterion 3 reads "within 0.2 dB" as |delta| <= 0.2.
- step 1b, when S1-S4 are present: the same table for S (cost from `cost_combo_step1b\summary.json`), and the byte
  identity of S's dumps to R0's by offset (`offset_identity`: the model's frames, offsets 0 and 1, must be identical).
  `S<n>_raw`'s OFAC dump is the model's main chain, not the carry chain (core `acc_dump_copies.cpp`), so its chain
  statistics equal R0_raw's; the carry chain itself is not dumped (step 1b, core e857106).
- step 1c, when S3, S5 and S6 are present: the same table with criterion 6 (the model's frames, offsets 0 and 1,
  byte-identical to R0 on all six views) and the cost from `cost_combo_step2c\summary.json`. From core d089024 the OFAC
  dump is v3 under a carry rule ≥ 1, and `chain[S<n>_raw]` reads its carry plane (`chain_plane[run]` = `carry`; a v2
  dump gives `model`).

## Short runs

`run_motion.py --short [W]` (default W = 32) starts the bench at frame S instead of 0 (`--start-frame S`) and dumps
the same window. The default (no flag) is unchanged.

- **S.** S = first dump − W, moved down to S ≡ P − 1 (mod 8), P from the view's R0. The core starts its cadence at
  the process's first evaluate (`core/temporal/controller.cpp` `PlanTemporal`: no residual yet → a full pass, then
  every 8 by `sinceFull`; `evalCounter` counts from the process start). The bench creates feature 18 after the first
  present, so the first evaluate is the run's second frame: frame = eval in a run from 0 (P = 1 on every view), frame =
  eval + S in a short run, full passes at S + 1 + 8k. Combo: S = 920 (W = 32), 888 (W = 64). The rule
  "(S − P) mod 8 == 0" would put them one frame late. The OFAC check moves the short run's evals by S
  (`check_run(..., offset)`); R0_s32 on combo: evals 929..1073 ≡ 1, the same full passes as R0.
- **Exposure.** `--short` replays the view's track `<out>\<view>\exposure.txt`, recorded once by a full run with
  `--exposure record` (`--exposure replay` replays it on a full run, cell `<run>_xrep`). Under `--auto-exposure 1` the
  meter reads the presented frame (after NR and the core), so each run records its own `exposure_<run>.txt`; the
  garden and lab views run at a fixed exposure (scale 1 on every frame), where one file serves the view. The bench
  refuses `--start-frame` under auto exposure without a track.
- **What else carries between frames** and what a short run does with it:

| State | Short run |
|---|---|
| auto exposure scale (Adapt's previous value, the firefly clamp's divisor) | replayed from the track, bit exact |
| auto exposure average / highlight targets | this frame's meter (in the track for reading only) |
| `--local-exposure` grid | none: rebuilt each frame from the frame and its scale |
| animation clock (`AnimationClock::Advance`, fixed 1/60) | advanced S − 1 times in the same steps before the loop |
| skinning previous pose (CPU and GPU) | evaluated at frame S − 1's time on frame S (equals the reused pose) |
| wind, camera (`CameraAt`), jitter (Halton of the frame), `--motion-span`, HUD, dump numbers | functions of the frame |
| previous view of frame S (motion guide) | frame S − 1's `CameraAt` (follow and interactive are rejected) |
| follow camera smoothing | rejected (`--camera follow`) |
| interactive state, menu, flight | rejected (`--interactive`) |
| probes, sky cube, lamp shadow setup | built once at start from frame 0's scene pose, as in the full run |
| Hi-Z occlusion, CSM, FXAA, `--ao clean` | per frame, no history |
| `--ao game` history | none on frame S (`AoHistoryUsable` needs frame − 1); converges with alpha 0.1 |
| path tracing accumulation | restarts at S; it already restarts on every moving frame |
| dynamic BLAS refit / rebuild every `--blas-rebuild` (path tracing) | counter starts at S: another rebuild phase |
| DLSS SR / RR history, NR feature 18 | created fresh, reset on frame S |
| core: cadence, residual, chains, model history | fresh; the cadence aligned by S |

- **Frame events** (`--dump`, `--resize-at`, `--render-switch-at`, `--recreate-swapchain`, `--nr-recreate`,
  `--nr-pause`, `--set-at`) before S are refused.

### Equivalence (combo, 2026-10-10, `rt_short` = rt_motion's runtime with this bench)

Runtime: `<root>\rt_short` (core b15d5d36, add-on 217040f3, bench 67ff8065); outputs `out\short\`.

| Check | Result |
|---|---|
| no flags, 40 frames, old bench against new (combo, and combo + `--auto-exposure 1 --local-exposure 1`) | 40/40 PNG byte-identical each |
| full run with `--exposure replay` against the recording run: combo T0, R0; combo + auto + local exposure T0 | 128/128 byte-identical each |
| `--nr off`, short (W = 32) against full | 128/128 byte-identical: the bench's frames are exact from S |
| T0 short against full, W = 32 / 56 / 64 / 184 | 41.9 / 41.9 / 41.9 / 42.0 dB, flat over the window (first 8 frames 41.6–42.1) |
| R0 short against full, W = 32 / 64 | 42.7 / 42.6 dB (offset 0: 40.8 / 40.7, offsets 1–7: 42.4–43.7) |
| combo + auto + local exposure T0 short (W = 32) against full | 43.0 dB |
| T0 short W = 32 against T0 short W = 64 (two starts) | 42.0 dB |
| R0 phase (OFAC) | short runs' full passes on R0's frames (evals ≡ 1 after the offset) |

PSNR never reaches 50 dB: with 184 frames of run-up it is where it is with 32. The difference is DLSS NR's own state
(with `--nr off` the frames are identical): the model's recurrent history keeps a ~42 dB trace of where it started,
and two short runs from different starts differ by as much as a short run and the full one. No run-up makes a short
run equal the full run; a short run is another sample of the NR trajectory, about 42 dB from the full run's.

What that does to the attribution numbers (R0 against T0, both short with the same S, combo):

| | whole | offset 0 | offset 7 | offsets 1–7 − 0 |
|---|---|---|---|---|
| full | 28.16 | 30.31 | 26.72 | −2.29 |
| short W = 32 | 28.21 | 30.43 | 26.73 | −2.36 |
| short W = 64 | 28.06 | 30.13 | 26.64 | −2.21 |

Time before the start work ("Start and run time" below) (wall, one run, 128 PNG dumps): T0 70 s → 50–54 s, R0 79–81 s → 49–50 s, `--nr off` 63 → 47 s: 1.4–1.6×,
not 5–10×. About 49 s of a combo run is fixed: the scene load (textures alone reach 17 s), ReShade and NR start, and
the 128 dumped frames; the 920 frames left out cost about 22 ms each (20–30 s).

**Recommendation.** W = 32 (more buys nothing). Compare short cells only with short cells of the same S (T0 and the
candidates all `--short 32`); never mix short and plain full cells in one comparison (a full cell with `--nr-reset 32`
is the short cell's window exactly, see below). Expect about ±0.2 dB on absolute
numbers against full runs (the spread between starts), so a criterion that hinges on 0.2 dB needs full runs or a
second S. The gain is about 20–30 s per combo run; the 176-frame views gain about 0.4 s and need no short mode.
`motion_metrics.py` still reads full cells (it numbers OFAC files by frame = eval).

### Exact short runs: the NR reset (`--nr-reset [W]`)

The ~42 dB above is the model's history from before S, not a flaw of the short run: a full run whose evaluate on frame
S + 1 (the short run's first evaluate, which always reads `DLSSNR.Reset=1`) also reads `DLSSNR.Reset=1` gives the short
run's frames byte for byte. Bench: `--nr-reset-at F[,F2...]` (frame F's feature-18 evaluate reads `DLSSNR.Reset=1`; the
add-on hands it to the core, which drops its temporal history, `PlanTemporal` → `Invalidate`, and to the model).
`run_motion.py --nr-reset [W]` puts it on S + 1 of `--short W` (cell `<run>_z<W>`). Combo, W = 32 (S = 920, reset 921):

| Check | Result |
|---|---|
| T0_z32 (full, reset) against T0_s32 (short) | 128/128 PNG byte-identical |
| R0_z32 against R0_s32 | 128/128 PNG byte-identical; OFAC payloads of the 19 common evals identical |
| T0_z32 / R0_z32 against T0 / R0 (no reset) | 41.9 / 42.7 dB: the reset itself is what the short run differs by |

The reset frame has no OFAC dump (the chain starts there; the check skips it). A short run with the same W is then the
exact window of a reset full run: compare short cells with `_z<W>` cells or with each other, not with plain full cells.
DLSS SR / RR keep their history under `--nr-reset-at`: on views with an upscaler (the lab's RR) a short run also resets
RR on frame S, which the flag does not reproduce.

### Variants (`run_variants.py`)

`run_variants.py --view combo --runs T0,R0 --short 32` runs the window [S, frames) once per run in one bench process
(`--start-frame S --variants K --variant-set ...`). Before variant k the bench starts the window again as a run from S
starts it: feature 18 released and created after frame S's present (so the add-on's core builds a new feature state),
the animation clock advanced S − 1 times from `--anim-start`, the frame's history reset, frame S − 1's view as the
previous one; then k's settings go through the add-on's `OptimizerFpsSetSettingV1`. Dumps are `v<k>_dump_<f>.png`;
the script moves them to `<run>_s<W>_mv` as `dump_<f>.png`, and the OFAC files by eval (each variant evaluates frames
S + 1 … frames − 1, the core numbers evals per process: `meta.json["eval_offset"]`), then runs the OFAC check.

- One ini for all variants: T0 is the first temporal run's ini with `TemporalMode=0`; the other keys that differ are set
  per variant, by the id the core's schema gives them (`ofps_settings_schema.h`, `schema.cpp`). The export takes
  persisted, non-diagnostic settings only (TemporalMode, TemporalEvery, ModelPasses, ColorFilter, TemporalModelMarker,
  ...): C1–C4, S1–S4, R0x and the R3 probes (diagnostic keys) and the oracle runs (bench flags) run alone. A refused
  setting fails the run. The export writes the keys it sets into ReShade.ini: the script restores the file afterwards.
- Raster views, fixed exposure (one `exposure.txt`), no `--raw`.

| Check (combo, W = 32) | Result |
|---|---|
| T0, R0 in one process (T0 first, and R0 first) against each separate short run | 128/128 PNG byte-identical each; R0's 19 OFAC payloads equal |
| wall | 17.5–24 s for both, against 11.4–11.5 s for one short run |

## Start and run time (combo, 2026-10-10)

`--startup-profile` prints the phases (bench `startup_profile.h`). The start to the first NR evaluate, warm file cache:

| Phase | before (bench 67ff8065) | after |
|---|---|---|
| loader, device, swap chain | 0.8 s | 0.9 s |
| glb, vertices, buffers, skinning, pose | 2.0 s | 2.2 s |
| HDRI | 0.5 s | 0.3 s |
| textures (369, 3.5 GB with mips) | 7.1–9.7 s | 1.2–1.8 s (cache) / 3.1–3.8 s (no cache) |
| BLAS, pipelines, sky, probes, lamps | 0.7 s | 1.1 s |
| first frames: NR runtime, feature 18 | 0.5 s | 0.9 s |
| to the first NR evaluate | 11.6–15.7 s | 5.8–7.9 s |

- Textures: the mips were 73 thread seconds; an 8-bit byte's linear value now comes from a table of the same expression
  (bit-exact, a hash test pins it). Decode, mips and staging (each texture's resource and upload buffer) run on a pool
  in any order; the main thread copies in image order, about 256 MB per wait. `--texture-cache` keeps the decoded mip
  chains (3.5 GB in 367 files); it stays uncompressed: XPRESS16K (2.1:1) read in 1.69–1.83 s against 1.18–1.53 s.
- Dumps: WIC's PNG encoder takes 162 ms per 1080p frame and 348 ms per 4K frame; 128 dumps held the run 15–21 s (1080p).
  They are written on worker threads now (main thread 0.2–0.7 s for 128), the files byte for byte the same.
- Not done: the glb geometry (2 s), the PSO cache (pipelines take 0.09 s: the driver caches them), BC textures (lossy).
  Times vary by ±30 % with the machine's load: compare runs made side by side.

Wall of one run (bench process; `final_base` = the bench before this branch, interleaved with this branch's):

| combo | before | after | frames |
|---|---|---|---|
| full T0 (1080 frames, 128 dumps) | 63.8 s | 33.3 s | 128/128 identical |
| full R0 (+ 134 OFAC dumps by the core) | 76.8 s | 44.7 s | 128/128 identical |
| short T0, W = 32 | 36.4 s | 11.4 s | 128/128 identical |
| short R0, W = 32 | 33.4 s | 11.5 s | 128/128 identical |
| variants T0 + R0 | – | 17.5 s (8.8 s a variant) | = the short runs |

Accuracy: a short run equals the full run with `--nr-reset` (`_z<W>`) byte for byte and a variant equals the short run
byte for byte; against a plain full run a short run stays 41.9 dB (T0) / 42.7 dB (R0), the NR history before S.
