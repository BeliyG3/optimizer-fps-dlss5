"""Runs of the camera-motion carry attribution (phase 3 step 0; motion/README.md).

  python run_motion.py --stage [--mode 3]
      Stages this checkout's bench and add-on: mode 1 into <root>\\rt_motion (run_foliage.stage: ReShade + the
      bench's NR, ReShade.ini from rt_foliage), mode 3 into rt_motion_bg (a copy of rt_foliage_bg, the RenoDX consumer,
      then the bench and Copy-CorePayload). rt_foliage* are only read. Old staged shaders move to out\\motion\\old.
  python run_motion.py --view V --run R [--mode 1|3] [--repeat k] [--raw] [--frames F] [--first S] [--out DIR]
      One run under <root>\\bench.lock into <out>\\<view>\\<run>[_m3][_raw][_r<k>]\\: dump_*.png (and with --raw
      motion_*.f32, depth_*.f32 and the OFAC window ofps_acc_*.bin), ReShade.ini, ReShade.log, bench stdout/stderr and
      meta.json. Run `prevdepth` (--nr off, the cadence motion, --dump-raw prevdepth) gives the exact disocclusion
      band to motion_metrics.py. R1, R2, R3a and prevdepth need the view's R0 first: P comes from its full-pass dumps,
      and every run's OFAC evals are checked against R0's (exit 1 and "ofac_check" in meta.json on a mismatch).
      Step 1: C1-C4 = R0 + DebugTemporalChainRule 1-4 (also with --raw, and --mode 3).
      Step 1b: S1-S4 = R0 + DebugTemporalCarryChainRule 1-4 (the model keeps rule 0; also with --raw, and --mode 3).
      Step 1c: S5, S6 = rules 5 and 6 (coherent change); with --raw the OFAC dump is v3 and carries the carry chain.
      Task 3: every run pins TemporalChainRule=0 (classic, run_foliage.BASE); D1 = R0 with the key absent (the shipped
      default 1: carry chain rule 5), which must equal S5 (also with --mode 3).
  ... --exposure record | replay      (mode 1, full run: the bench records the view's exposure track, or replays it)
  ... --short [W]                     (mode 1: from frame S = first - W aligned to R0's phase, the track replayed; cell
                                       <run>..._s<W>; README "Short runs")
"""
import argparse, glob, json, os, shutil, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "foliage"))
import run_foliage as rf                                    # noqa: E402
from motion_ofac import check_run                          # noqa: E402

RT = os.path.join(rf.ROOT, "rt_motion")
RT_BG = os.path.join(rf.ROOT, "rt_motion_bg")
OUT = os.path.join(rf.ROOT, r"out\motion")
N = 8
WARMUP = 32                                                # --short: frames run before the first dump (README "Short runs")
TEXTURE_CACHE = os.path.join(rf.ROOT, "cache", "textures")    # bench --texture-cache: the decoded mips (no effect on the frames)
MIN_FREE_GB = 60                                           # plan: keep W: above 60 GB free (raw runs are GBs)
T1 = dict(TemporalMode="1", TemporalEvery=str(N), DebugTemporalDumpAcc="1")
RUNS = {   # run: (ini keys, oracle: the bench's motion is the offset to the last full pass)
    "T0": (dict(TemporalMode="0"), False),
    "R0": (T1, False),
    "R0x": (dict(T1, DebugTemporalNoExpect="1"), False),
    "R1": (dict(T1, DebugTemporalCarryProbe="1"), True),
    "R2": (dict(T1, DebugTemporalCarryProbe="3"), True),
    "R3a": (dict(T1, DebugTemporalCarryProbe="7"), True),
    "R3b": (dict(T1, DebugTemporalCarryProbe="4"), False),
    # step 1: R0 + a chain restart rule (core PwTLinkFails; 0 = today, 1 never, 2 colour needs depth, 3 = 2 + 5x5 vote,
    # 4 depth only); applies to every chain the core builds, mode 3 included
    **{f"C{n}": (dict(T1, DebugTemporalChainRule=str(n)), False) for n in range(1, 5)},
    # step 1b: R0 + a carry chain by that rule for the reprojection alone; the model's chain stays rule 0; step 1c adds
    # 5 (coherent colour: a colour fail restarts only with >= 18 of 5x5 colour fails) and 6 (5 + a coherent Refine shift)
    **{f"S{n}": (dict(T1, DebugTemporalCarryChainRule=str(n)), False) for n in range(1, 7)},
    "D1": (dict(T1, TemporalChainRule=None), False),      # task 3: the shipped TemporalChainRule default (absent = 1)
    "mask": ({}, False),                                   # the regions: --nr off --view vegmask (raster views)
    "prevdepth": ({}, True),                               # the exact band: --nr off --dump-raw prevdepth (motion_band.py)
}
OUTPUTS = rf.OUTPUTS + ("prevdepth_*.f32",)
# never leak between runs; TemporalModelMarker absent = the shipped default (2), i.e. today's master
UNSET = rf.DEBUG_KEYS + ("DebugTemporalNoExpect", "DebugTemporalCarryProbe", "DebugTemporalDumpAccFrom", "TemporalModelMarker",
                         "DebugTemporalChainRule", "DebugTemporalCarryChainRule", "TemporalChainRule")
CANDIDATES = tuple(f"C{n}" for n in range(1, 5))
CARRY_CANDIDATES = tuple(f"S{n}" for n in range(1, 7))   # step 1b (S1-S4), step 1c (S5, S6)
RULE_KEYS = ("DebugTemporalChainRule", "DebugTemporalCarryChainRule", "TemporalChainRule")
BASE = {k: v for k, v in rf.BASE.items() if k != "TemporalModelMarker"}
GARDEN = ("combo", "pan", "held")
VIEWS = {  # view: (bench arguments, frames, first dump) as in phase 1 (run_foliage.py, FOLIAGE_DIAG.md) and phase 2
    "combo": (rf.VIEWS["combo"], 1080, 952), "pan": (rf.VIEWS["pan"], 176, 48), "held": (rf.VIEWS["held"], 176, 48),
    "lab_combo": (rf.VIEWS["lab_combo"], 176, 48),
    "lab_forward": (rf.LAB + ["--camera", "forward", "--move-speed", "7"], 176, 48),   # temporal_quality.py's forward
    "ghost_door": (rf.VIEWS["lab_ghost_door"], 176, 48),
    # path tracing hands the same motion guide to DLSS RR: with the oracle's cadence motion RR reprojects wrongly and the
    # colour the core gets is not T0's, so R1/R2 there are invalid; the lab cameras in raster (no upscaler) carry them
    "lab_combo_raster": (rf.LAB_RASTER + ["--camera", "combo"], 176, 48),
    "lab_forward_raster": (rf.LAB_RASTER + ["--camera", "forward", "--move-speed", "7"], 176, 48),
    # step 1c: the same flight over a matte floor (the reflection hypothesis). The bench has no roughness flag; the later
    # --gltf loads a copy of lab_scene.glb whose lab_tex_shell_floor roughnessFactor is 1.0 instead of 0.12 (out\motion\matte)
    "lab_forward_raster_matte": (rf.LAB_RASTER + ["--camera", "forward", "--move-speed", "7", "--gltf",
                                                  os.path.join(OUT, r"matte\lab_scene_matte_floor.glb")], 176, 48),
}


def park_shaders(rt):
    """Restaging overwrites the bench and Copy-CoreShaders deletes the staged *.dxbc: keep the previous ones in
    out\\motion\\old\\stage_<time>\\<runtime> first (the exe and shaders\\bin copied, the dxbc moved)."""
    dest = os.path.join(OUT, "old", f"stage_{time.strftime('%Y%m%d-%H%M%S')}", os.path.basename(rt))
    if os.path.exists(os.path.join(rt, "pw_bench12.exe")):
        os.makedirs(dest, exist_ok=True)
        shutil.copy2(os.path.join(rt, "pw_bench12.exe"), dest)
        shutil.copytree(os.path.join(rt, r"shaders\bin"), os.path.join(dest, r"shaders\bin"), dirs_exist_ok=True)
    rf.move_all(os.path.join(rt, "optimizer-fps-dlss5"), ("*.dxbc",), dest)


def stage(mode):
    rt = RT_BG if mode == 3 else RT
    park_shaders(rt)
    if mode == 3:
        if not os.path.exists(rt):
            shutil.copytree(os.path.join(rf.ROOT, "rt_foliage_bg"), rt, ignore=shutil.ignore_patterns(
                "dump_*", "ofps_acc_*", "motion_*", "depth_*", "*.log", "out", "old", "optimizer-fps-dlss5"))
        os.makedirs(os.path.join(rt, r"shaders\bin"), exist_ok=True)
        shutil.copy2(os.path.join(rf.BENCH, "pw_bench12.exe"), rt)
        for cso in glob.glob(os.path.join(rf.BENCH, r"shaders\bin\*.cso")):
            shutil.copy2(cso, os.path.join(rt, r"shaders\bin"))
        build, payload = os.path.join(rf.REPO, r"out\build\x64"), os.path.join(rf.REPO, r"tools\Runtime-Payload.ps1")
        subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-Command",
                        f". '{payload}'; Copy-CorePayload '{build}' '{rt}'"], check=True)
    else:
        if not os.path.exists(os.path.join(rt, "ReShade.ini")):
            os.makedirs(rt, exist_ok=True)
            shutil.copy2(os.path.join(rf.RT, "ReShade.ini"), rt)     # phase 1-2's ini, not the dev checkout's
        rf.stage(rt)
    names = ("pw_bench12.exe", "optimizer-fps-dlss5.addon64", "optimizer-fps-dlss5-core.dll",
             "nvngx.dll_optimizerfps.dll", "dxgi.dll")
    record = {n: rf.sha256(os.path.join(rt, n)) for n in names}
    record["dxbc"] = len(glob.glob(os.path.join(rt, "optimizer-fps-dlss5", "*.dxbc")))
    os.makedirs(OUT, exist_ok=True)
    json.dump(dict(record, runtime=rt, time=time.strftime("%Y-%m-%d %H:%M:%S")),
              open(os.path.join(OUT, f"stage_{os.path.basename(rt)}.json"), "w"), indent=1)
    print(json.dumps(record, indent=1))
    return 0


def compact(folder, patterns=("*.f32", "*.bin")):
    """Lossless NTFS compression (XPRESS16K) of a run's raw dumps and OFAC files (PNG is already deflate); files already
    compressed are skipped. One line: compact's summary. The texture cache stays plain: compressed, its warm reads took
    1.69-1.83 s against 1.18-1.53 s (combo, 3 runs each; README "Start")."""
    r = subprocess.run(["compact", "/c", "/q", "/i", "/exe:xpress16k", f"/s:{folder}", *patterns], capture_output=True, text=True)
    lines = [l.strip() for l in r.stdout.splitlines() if l.strip()]
    print(f"compact {folder} {' '.join(patterns)}: exit {r.returncode}; {' '.join(lines[-2:])}", flush=True)


def cell_name(run, mode, raw, repeat, short=None, exposure=None, reset=None):
    return (run + ("_m3" if mode == 3 else "") + ("_raw" if raw else "") + (f"_s{short}" if short else "") +
            (f"_z{reset}" if reset else "") + ("_xrep" if exposure == "replay" else "") + (f"_r{repeat}" if repeat > 1 else ""))


def short_start(first, warmup, phase, n=N):
    """--short: the first frame S <= first - warmup with S = P - 1 (mod n). The core starts its cadence at the process's
    first evaluate, which is a full pass (no residual yet; core/temporal/controller.cpp PlanTemporal) and comes on the
    run's second frame (frame = eval in a run from 0, so P = 1 there): from S the full passes are S + 1 + n*k, R0's frames."""
    s = first - warmup
    return s - (s - (phase - 1)) % n


def exposure_track(out, view, name, args):
    """The view's exposure track (bench --exposure-record / --exposure-replay). The meter reads the presented frame, after
    NR and the core, so under --auto-exposure 1 every run records its own; without it every scale is 1, one file a view."""
    auto = [args[i + 1] for i, a in enumerate(args[:-1]) if a == "--auto-exposure"]
    return os.path.join(out, view, f"exposure_{name}.txt" if auto and auto[-1] == "1" else "exposure.txt")


def plan(view, name, mode, raw, frames, first, out):
    """-> (ini keys, bench flags, dump range start, OFAC window or None, P or None); ValueError on a bad combination."""
    keys, oracle = RUNS[name]
    if mode == 3 and (name not in ("T0", "R0", "D1") + CANDIDATES + CARRY_CANDIDATES or raw):
        raise ValueError("mode 3 runs only T0, R0, D1, C1-C4 and S1-S6, without --raw (the oracle and the dumps need TemporalMode 1)")
    if name == "mask" and (raw or mode == 3 or view not in GARDEN):
        raise ValueError("mask: garden views only (vegmask is raster), mode 1, no --raw")
    if name == "prevdepth" and (raw or mode == 3):
        raise ValueError("prevdepth: mode 1, no --raw (it is a raw dump of its own)")
    if name == "mask":
        return {}, ["--nr", "off", "--view", "vegmask"], first, None, None
    if mode == 3:   # TemporalChainRule pinned as in mode 1 unless the run removes it (None)
        rule = {k: v for k, v in dict(BASE, **keys).items() if k in RULE_KEYS and v is not None}
        keys = dict(TemporalMode="0" if name == "T0" else "3", TemporalEvery=str(N), TemporalDebugLog="1", **rule)
        return keys, ["--upscaler", "sr"] if view in GARDEN else [], first, None, None   # RenoDX needs DLSS
    keys = {k: v for k, v in dict(BASE, **keys).items() if v is not None}   # None: the key stays out of the ini
    flags, phase, window, start = ["--nr", "native"], None, None, first
    r0 = os.path.join(out, view, "R0")
    if (keys.get("DebugTemporalDumpAcc") or oracle) and (name != "R0" or raw) and not os.path.isdir(r0):
        raise ValueError(f"run R0 of {view} first: P and the OFAC check come from {r0}")
    if oracle:
        phase = check_run(r0, r0)["phase"]
        flags += ["--motion-span", "cadence", "--cadence", str(N), "--cadence-phase", str(phase)]
    if name == "prevdepth":
        return {}, ["--nr", "off"] + flags[2:] + ["--dump-raw", "prevdepth"], first, None, phase
    if raw:
        flags += ["--dump-raw", "motion,depth"]
        keys["DebugTemporalDumpAccFrom"] = str(first)
        window, start = [first, min(first + 64, frames)], max(first - N, 0)   # the band reads the depth N back
    return keys, flags, start, window, phase


def window_start(out, view, name, start, warmup):
    """S of --short / --nr-reset W: R0's phase places it (T0 may run first: P = 1, every view's)."""
    r0 = os.path.join(out, view, "R0")
    if name != "T0" and not os.path.isdir(r0):
        raise ValueError(f"run R0 of {view} first (its phase places S)")
    return short_start(start, warmup, check_run(r0, r0)["phase"] if os.path.isdir(r0) else 1)


def run(view, name, mode, repeat, raw, frames, first, out, short=None, exposure=None, runtime=None, reset=None, extra=(),
        no_compact=False):
    args, vf, vfirst = VIEWS[view]
    frames, first = frames or vf, vfirst if first is None else first
    keys, flags, start, window, phase = plan(view, name, mode, raw, frames, first, out)
    track, begin = exposure_track(out, view, name, args), 0
    if (short or exposure) and mode != 1:
        raise ValueError("--short and --exposure run in mode 1 only")
    if (short or reset) and mode != 1:
        raise ValueError("--short and --nr-reset run in mode 1 only")
    resets = []
    if reset:                                               # the short run's first evaluate (S + 1) resets the model
        resets = [window_start(out, view, name, start, reset) + 1]
        flags += ["--nr-reset-at", str(resets[0])]
    if short:
        begin = window_start(out, view, name, start, short)
        if begin < 0 or exposure == "record" or not os.path.exists(track):
            raise ValueError(f"--short needs {short} frames before the dumps and {track} (a full run with --exposure record)")
        exposure = "replay"
        flags += ["--start-frame", str(begin)]
        if "DebugTemporalDumpAccFrom" in keys:              # the core numbers its evals from the run's start
            keys["DebugTemporalDumpAccFrom"] = str(first - begin)
    if exposure:
        flags += [f"--exposure-{exposure}", track]
    flags += ["--texture-cache", TEXTURE_CACHE] + list(extra)   # --bench-arg: e.g. --startup-profile (no effect on the frames)
    rt = runtime or (RT_BG if mode == 3 else RT)
    free_gb = shutil.disk_usage(out if os.path.exists(out) else rf.ROOT).free / 2 ** 30
    if free_gb < MIN_FREE_GB:
        raise SystemExit(f"W: has {free_gb:.0f} GB free, below {MIN_FREE_GB} GB: not running")
    unset = [k for k in UNSET if k not in keys]
    cell = os.path.join(out, view, cell_name(name, mode, raw, repeat, short, exposure if not short else None, reset))
    exe, ini = os.path.join(rt, "pw_bench12.exe"), os.path.join(rt, "ReShade.ini")
    argv = [exe, str(frames)] + args + flags + ["--dump", ",".join(map(str, range(start, frames)))]
    os.makedirs(rf.TMP, exist_ok=True)
    waited = rf.take_lock(f"motion run_motion.py {view} {os.path.basename(cell)}")
    try:
        stamp = time.strftime("%Y%m%d-%H%M%S")
        old = os.path.join(out, "old", stamp)
        if os.path.exists(cell):
            os.makedirs(os.path.join(old, view), exist_ok=True)
            shutil.move(cell, os.path.join(old, view, os.path.basename(cell)))
        if exposure == "record" and os.path.exists(track):
            os.makedirs(os.path.join(old, view), exist_ok=True)
            shutil.move(track, os.path.join(old, view, os.path.basename(track)))
        if rf.move_all(rt, OUTPUTS, os.path.join(old, "stale_rt")):
            print(f"stale outputs in {rt} moved to {old}\\stale_rt")
        os.makedirs(cell)
        rf.set_ini(ini, keys, unset)
        t0 = time.time()
        with open(os.path.join(cell, "bench.out.txt"), "w") as so, open(os.path.join(cell, "bench.err.txt"), "w") as se:
            proc = subprocess.Popen(argv, cwd=rt, stdout=so, stderr=se, env=dict(os.environ, TEMP=rf.TMP, TMP=rf.TMP),
                                    creationflags=0x08000000)
            try:
                code = proc.wait(rf.TIMEOUT_S)
            except subprocess.TimeoutExpired:
                proc.kill()                                # only the PID this script started
                code = "timeout"
        wall = round(time.time() - t0, 1)
        counts = {p: len(glob.glob(os.path.join(rt, p))) for p in OUTPUTS}
        rf.move_all(rt, OUTPUTS, cell)
        shutil.copy2(ini, os.path.join(cell, "ReShade.ini"))
        if os.path.exists(os.path.join(rt, "ReShade.log")):
            shutil.copy2(os.path.join(rt, "ReShade.log"), os.path.join(cell, "ReShade.log"))
        rf.set_ini(ini, {}, UNSET)                         # no probe left in the runtime for the next run
    finally:
        os.remove(rf.LOCK)
    ofac = mode == 1 and bool(keys.get("DebugTemporalDumpAcc"))
    meta = dict(view=view, run=name, cell=cell, mode=mode, raw=raw, repeat=repeat, frames=frames, dumps=[start, frames - 1],
                acc_window=window, phase=phase, ofac=ofac, start_frame=begin, exposure=exposure, nr_reset=reset, argv=argv, ini=keys, unset=unset, exe_sha256=rf.sha256(exe),
                addon_sha256=rf.sha256(os.path.join(rt, "optimizer-fps-dlss5.addon64")),
                core_sha256=rf.sha256(os.path.join(rt, "optimizer-fps-dlss5-core.dll")),
                time=time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(t0)), wall_s=wall, pid=proc.pid, exit=code,
                lock_wait_s=waited, files=counts)
    if ofac and code == 0:
        r0 = os.path.join(out, view, "R0")
        try:
            meta["ofac_check"] = check_run(cell, cell if cell == r0 else r0, N, window, begin, resets)
        except ValueError as e:
            meta["ofac_check"], code = f"ABORT: {e}", "ofac_check"
    json.dump(meta, open(os.path.join(cell, "meta.json"), "w"), indent=1)
    if not no_compact:
        compact(cell)
    print(json.dumps({k: meta.get(k) for k in ("view", "run", "cell", "exit", "wall_s", "files", "ofac_check")}))
    return 0 if code == 0 else 1


def main():
    rf.bench_paths.require_root()
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--stage", action="store_true")
    ap.add_argument("--view", choices=sorted(VIEWS))
    ap.add_argument("--run", choices=list(RUNS))
    ap.add_argument("--mode", type=int, default=1, choices=(1, 3))
    ap.add_argument("--repeat", type=int, default=1)
    ap.add_argument("--raw", action="store_true", help="--dump-raw motion,depth and the OFAC window from the first dump")
    ap.add_argument("--frames", type=int, help="default: the view's (combo 1080, others 176)")
    ap.add_argument("--first", type=int, help="first dump frame (default: the view's, combo 952, others 48)")
    ap.add_argument("--out", default=OUT)
    ap.add_argument("--runtime", help="a staged runtime folder instead of rt_motion / rt_motion_bg")
    ap.add_argument("--exposure", choices=("record", "replay"), help="full run: record or replay the view's exposure track")
    ap.add_argument("--short", type=int, nargs="?", const=WARMUP, metavar="W",
                    help=f"start W (default {WARMUP}) frames before the first dump, aligned to R0's phase, the track replayed")
    ap.add_argument("--nr-reset", type=int, nargs="?", const=WARMUP, metavar="W",
                    help="DLSSNR.Reset=1 on frame S + 1 (S as --short W): a full run whose window starts as a short run's")
    ap.add_argument("--no-compact", action="store_true", help="leave the raw dumps and OFAC files uncompressed")
    ap.add_argument("--bench-arg", action="append", default=[], metavar="ARG", help="one more bench argument (repeatable)")
    a = ap.parse_args()
    if a.stage:
        return stage(a.mode)
    if not a.view or not a.run:
        ap.error("--view and --run are required (or --stage)")
    try:
        return run(a.view, a.run, a.mode, a.repeat, a.raw, a.frames, a.first, a.out, a.short, a.exposure, a.runtime, a.nr_reset, a.bench_arg,
                   a.no_compact)
    except ValueError as e:
        ap.error(str(e))


if __name__ == "__main__":
    sys.exit(main())
