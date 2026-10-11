"""The background mode (TemporalMode 3) on bench12, where it actually runs (final review of phase 2).

rt_foliage (ReShade + the bench's own NR, run_nrhost) has no registered host queue on the evaluate's device, so
TemporalMode 3 falls back to the synchronous interpolation there (ReShade.log: "background mode unavailable here").
rt_foliage_bg is deploy_addon.ps1's RenoDX-consumer runtime (as run_addon's background reports): deploy_addon.ps1
-Build <this checkout>\\out\\build\\x64 -Runtime <root>\\rt_foliage_bg, then this checkout's pw_bench12.exe and
shaders\\bin\\*.cso copied over run_fork's. Every comparison stays inside this runtime: T0, classic
(TemporalModelMarker=1) and the default (key absent = 2).

  python run_background.py lab <label> <marker 1|default>     temporal_quality.py follow N = 0, 8, two runs, mode 3
  python run_background.py garden <case t0|t3|t3def> <repeat>  the garden held view, --upscaler sr (RenoDX needs DLSS)
Each run takes bench.lock; outputs under out\\foliage_final\\bg\\. Metrics: repeats.py lab / garden (vegmask of
out\\foliage\\held\\mask\\r1).
"""
import os, shutil, subprocess, sys, time

import run_foliage as rf

RT = os.path.join(rf.ROOT, "rt_foliage_bg")
OUT = os.path.join(rf.ROOT, r"out\foliage_final\bg")
TQ = os.path.join(rf.BENCH, "temporal_quality.py")
INI = os.path.join(RT, "ReShade.ini")


def marker(value):
    keys = {} if value == "default" else {"TemporalModelMarker": value}
    rf.set_ini(INI, keys, [] if keys else ["TemporalModelMarker"])


def lab(label, value):
    rf.take_lock(f"foliage run_background.py lab {label}")
    try:
        marker(value)
        code = subprocess.call([sys.executable, TQ, label, "--runtime", RT, "--mode", "3", "--scenarios", "follow",
                                "--cadences", "0,8", "--runs", "2", "--dump-format", "png", "--extra",
                                f"--gltf {rf.ASSETS}\\lab_scene.glb"], cwd=rf.BENCH)
        os.makedirs(OUT, exist_ok=True)
        shutil.move(os.path.join(RT, label), os.path.join(OUT, label))
        shutil.copy2(INI, os.path.join(OUT, label, "ReShade.base.ini"))
    finally:
        os.remove(rf.LOCK)
    return code


def garden(case, repeat):
    keys = {"t0": {"TemporalMode": "0"}, "t3": {"TemporalMode": "3", "TemporalEvery": "8"},
            "t3def": {"TemporalMode": "3", "TemporalEvery": "8"}}[case]
    cell = os.path.join(OUT, "held", case, f"r{repeat}")
    argv = [os.path.join(RT, "pw_bench12.exe"), "176"] + rf.VIEWS["held"] + ["--upscaler", "sr", "--dump",
                                                                            ",".join(map(str, range(48, 176)))]
    rf.take_lock(f"foliage run_background.py garden {case} r{repeat}")
    try:
        marker("default" if case == "t3def" else "1")
        rf.set_ini(INI, dict(keys, TemporalDebugLog="1"), [])
        if os.path.exists(cell):
            shutil.move(cell, os.path.join(OUT, "old", time.strftime("%Y%m%d-%H%M%S"), case, f"r{repeat}"))
        rf.move_all(RT, ("dump_*.png",), os.path.join(OUT, "old", "stale_rt"))
        os.makedirs(cell)
        with open(os.path.join(cell, "bench.out.txt"), "w") as so:
            proc = subprocess.Popen(argv, cwd=RT, stdout=so, stderr=subprocess.STDOUT, creationflags=0x08000000)
            try:
                code = proc.wait(rf.TIMEOUT_S)
            except subprocess.TimeoutExpired:
                proc.kill()                                      # only the PID this script started
                code = "timeout"
        rf.move_all(RT, ("dump_*.png",), cell)
        for name in ("ReShade.ini", "ReShade.log"):
            shutil.copy2(os.path.join(RT, name), os.path.join(cell, name))
    finally:
        os.remove(rf.LOCK)
    print(case, repeat, "exit", code, "dumps", len([f for f in os.listdir(cell) if f.startswith("dump_")]), flush=True)
    return 0 if code == 0 else 1


if __name__ == "__main__":
    rf.bench_paths.require_root()
    if sys.argv[1] == "lab":
        sys.exit(lab(sys.argv[2], sys.argv[3]))
    sys.exit(garden(sys.argv[2], int(sys.argv[3])))
