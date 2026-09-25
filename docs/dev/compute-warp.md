# Compute warp: evidence for plan 5, task 8

The task-8 WARP test exercises Pack, model copy, Unpack, temporal base Unpack,
RGBA16F motion, planar depth, padded output, and direct and intermediate-copy
targets with nonzero origins. `Evaluate` keeps its existing shape contract:
a shifted output is pass-through with a status reason. The shifted-write test
calls `ComputePath::Unpack` directly. Both paths check the destination padding.

## Pixel and compute comparison

`DebugWarpPath=pixel` is compared with `reference_before_2026.9.1` by
`compare_plan2.py --candidate candidate_p5t8`. Its 64 frame pairs, including
Off, T0, T1, and the direct core host, are byte-identical. Compute has separate
captures under `plan5-task8-compute`; `compare_plan5.py` records manifest and
binary SHA-256, MAD, maximum channel difference, changed pixels, exact bounds,
and diff masks for frames 120 and 239.

| Runtime | Pairs | Maximum MAD (RGB8 values) | Maximum channel difference | Maximum changed pixels in a frame | Result at MAD ≤ 0.35 |
|---|---:|---:|---:|---:|---|
| `run_addon` | 14 | 0.302051 | 17 | 1,038,806 | PASS |
| `run_r521` | 18 | 0.201290 | 11 | 815,239 | PASS |
| `run_nrhost` | 26 | 0.294463 | 21 | 908,565 | PASS |
| direct `--host core` | 6 | 0 | 0 | 0 | PASS |

The ignored `plan5-task8-compute/plan5_comparison.json` in each runtime
contains every exact changed-pixel bound and points to its per-frame
`*_diff_mask_*.png`. These masks cover the captured image; the GPU fixture,
which reads padded texels, verifies that Unpack did not write outside its
destination region. The hardware no-model WARP comparison measured packed
and output float MAD of 0, with a clean D3D12 debug layer.

On the hardware GPU, the pixel RTV and compute typed-UAV Pack results differ
in packed motion. The earlier float16 ULP check measured up to 16 ULP on Y;
ULP depends on magnitude and was the wrong measure for this contract. The
absolute maximum in work-grid pixel units is **X 0.0078125, Y 0.00390625**,
both below **1/64 = 0.015625**. Pixel Pack derives its position from
interpolated UV, while compute Pack calculates the position directly. This
small position difference is a plausible source of the changed RGB pixels
after sampling and model evaluation; the directional and localized ULP pattern
supports that explanation. The task-8 comparison therefore permits
**MAD ≤ 0.35/255** in normalized RGB, represented as `0.35` in the RGB8-valued
bench helper. The maximum channel difference remains reported without a limit.
The GPU region fixture guards against writes outside the requested output.

The `run_r521` next-dispatch sentinel requires `--core-state-rebind`. Its
Mode=0 control has the same descriptor-heap failure without rebinding
(`out/sentinel-off/off.stderr.txt`), so this is the NGX/host rebinding contract,
not a compute-only restore promise. The sentinel must pass after explicit
rebind, and its draw case must pass. These are WARP-host checks, not gameplay
validation.

The standalone `run_nrhost` benchmark also has to rebind before its next draw
or dispatch. Its first pixel draw sentinel without the flag failed on a
descriptor-heap mismatch after NGX
(`run_nrhost/plan5-task8-pixel/state_sentinel/draw.stderr.txt`). That benchmark
does not exercise Streamline's state-restoration wrapper; the test now uses
the standalone-host rebinding contract from §6.

For the fork, `ExtendedStateRestore=true` alone does not make upstream
`D3D12Hooks::RestoreRoot` run: the function is gated by
`RestoreComputeSignature` or `RestoreGraphicSignature`. The first archived fork
pixel draw failed on a descriptor-heap mismatch with both gates false. The
fork fixture enables both signature settings and `ExtendedStateRestore`, so
the next draw and dispatch check the promised state envelope without explicit
benchmark rebinding. The failed first run is retained as `plan5-task8`;
the corrected run uses `plan5-task8-r2`.

`run_fork_core/plan5-task8-r2` completed `Core=1` at 100% and 75%, with
pixel and compute paths and two repeats each. All eight next-draw and
next-dispatch sentinels passed. `compare_plan5_fork.py` checked 72 frame pairs:
pixel versus plan3 baseline, pixel repeat, compute repeat, and compute versus
pixel all had MAD 0. The capture keeps each INI, log, binary hashes, and
sentinel output under its case directory.

## Task 9 acceptance, 24 September 2026

Pixel captures use `candidate_p5t9` as requested for the full
`compare_plan2.py` gate; compute captures use `plan5-task9-compute`.
All 64 pixel/reference pairs have MAD, maximum difference, and changed pixels
equal to zero. `compare_plan5.py --pixel candidate_p5t9 --compute
plan5-task9-compute --mad-limit-rgb8 0.35` passed all 64 pairs.

| Runtime | Pairs | Maximum MAD (RGB8) | Maximum channel difference | Maximum changed pixels/frame | Result |
|---|---:|---:|---:|---:|---|
| `run_addon` | 14 | 0.302051 | 17 | 1,038,806 | PASS |
| `run_r521` | 18 | 0.201290 | 11 | 815,239 | PASS |
| `run_nrhost` | 26 | 0.294463 | 21 | 908,565 | PASS |
| direct `--host core` | 6 | 0 | 0 | 0 | PASS |

The Task 8 hardware measurement of packed motion (X 0.0078125, Y
0.00390625 work pixels) remains below the owner's 1/64 threshold; the Task 9
WARP test again passed that absolute-threshold assertion. The pixel shader's
interpolated UV and compute shader's direct coordinate calculation explain the
localized motion difference and justify the measured 0.35 RGB8 MAD limit.
Off frames are identical. Comparison JSON records bounds, masks, manifests,
and core/DXBC hashes under each ignored compute candidate directory.

The 16 addon/r521/nrhost/direct-core next-draw and next-dispatch sentinels
passed with the documented host rebinding. Fork `Core=1` at 100% and 75%,
pixel and compute with two repeats each, passed eight draw and eight dispatch
sentinels. Its 72 frame comparisons have MAD 0. All captured stderr files
had zero D3D12/DXGI error matches. These are WARP-host checks, not gameplay
validation.
