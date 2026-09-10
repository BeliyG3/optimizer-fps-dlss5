# Bench baseline for the 26.26 clean-up (stage 27 B0)

Build: add-on 26.25 (deployed static build, sha256 of the BG3 copy), 13 dxbc. Bench: default
procedural scene, `--fps-cap 60`, 232 frames, dumps 200-219, Mode=2 (Peripheral 80/90), CrashGuard=0.
Metrics: `errflick.py <every> <mode> 200 219` (MAD luma against the every-frame run; "error change"
is the flicker component).

| Run | temporal | err centre | err periphery | change centre (max) | change periphery (max) |
|---|---|---|---|---|---|
| base_sync4 | 1,4 | 2.762 | 2.992 | 0.889 (0.988) | 0.992 (1.266) |
| base_async | 3,2 | 1.836 | 1.886 | 0.879 (0.928) | 0.969 (1.114) |

Every step of stage B must stay within noise of these numbers (±0.05); the shader compaction
(B6) must reproduce the dumps bit-exactly (`compare_dumps.py`). Dumps live in
`$BENCH_RUN/base_every`, `base_sync4`, `base_async` (not in git).

Note: the bench ends with TerminateProcess, so `run_modes.sh` deletes `optimizer-fps-dlss5.session`
and writes `CrashGuard=0`; otherwise the crash guard forwards every call untouched and the
metrics silently compare pass-through against pass-through.
