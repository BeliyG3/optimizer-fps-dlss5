"""Several short runs of one view in one bench process (README "Short runs", "Variants").

  python run_variants.py --view V --runs T0,R0[,...] [--short W] [--out DIR] [--runtime DIR] [--bench-arg ARG ...]

The bench (--start-frame S --variants K) plays the window [S, frames) K times; before variant k it starts the window
again as a run from S would (feature 18 released and created after frame S's present, the clock and the history reset)
and sets k's add-on settings through OptimizerFpsSetSettingV1 (--variant-set). Variant k's cell is <run>_s<W>_mv: its
dumps, its OFAC files, a copy of the bench's logs and meta.json, as a separate `run_motion.py --short W` cell would hold.

The variants share one ini and one set of bench flags: they may differ only in settings the export accepts (persisted,
not diagnostic: TemporalMode, TemporalEvery, ModelPasses, ColorFilter, TemporalModelMarker, ...). T0 is the first
temporal run's ini with TemporalMode=0. C1-C4, S1-S4, R0x and the R3 probes differ in diagnostic keys and the oracle runs
in bench flags: those run alone. ReShade.ini is restored after the run (the export writes the keys it sets).
"""
import argparse, glob, json, os, re, shutil, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import run_motion as rm                                    # noqa: E402
from run_motion import rf, check_run                        # noqa: E402

SCHEMA_H = os.path.join(rf.REPO, r"core\api\ofps_settings_schema.h")
SCHEMA_CPP = os.path.join(rf.REPO, r"core\settings\schema.cpp")


def setting_ids():
    """{ini key: OfpsSettingId} from the core's schema (the enum's order and the rows' keys)."""
    header = open(SCHEMA_H, encoding="utf-8").read()
    body = re.sub(r"//[^\n]*", "", header[header.index("OFPS_SET_MODE = 0"):header.index("OFPS_SET_COUNT")])
    order = re.findall(r"OFPS_SET_\w+", body)
    rows = re.findall(r'Row\((OFPS_SET_\w+),\s*"(\w+)"', open(SCHEMA_CPP, encoding="utf-8").read())
    return {key: order.index(name) for name, key in rows}


def variant_keys(view, names, frames, first, out):
    """-> (the shared ini keys, the bench flags, dump start, [(name, {key: value} set before the variant)])."""
    plans = {n: rm.plan(view, n, 1, False, frames, first, out) for n in names}
    for n, (_, flags, _, window, _) in plans.items():
        if window is not None or flags != plans[names[0]][1]:
            raise ValueError(f"{n}: variants share the bench flags and take no OFAC window (run it alone)")
    temporal = [n for n in names if n != "T0"]
    base = dict(plans[temporal[0] if temporal else names[0]][0])
    ids, sets = setting_ids(), []
    for n in names:
        keys = dict(base, TemporalMode="0") if n == "T0" else plans[n][0]
        diff = {k: keys.get(k) for k in set(keys) | set(base) if keys.get(k) != base.get(k)}
        if any(v is None or k not in ids for k, v in diff.items()):
            raise ValueError(f"{n}: keys {sorted(diff)} differ from the shared ini and cannot be set at run time")
        sets.append((n, diff))
    return base, plans[names[0]][1], plans[names[0]][2], sets


def run_variants(view, names, short, out, runtime=None, extra=(), no_compact=False):
    args, frames, first = rm.VIEWS[view]
    base, flags, start, sets = variant_keys(view, names, frames, first, out)
    begin = rm.window_start(out, view, "T0" if names == ["T0"] else "R0", start, short)
    track = rm.exposure_track(out, view, "T0", args)
    if not os.path.exists(track) or os.path.basename(track) != "exposure.txt":
        raise ValueError(f"{track}: variants need the view's one track (fixed exposure; a full run with --exposure record)")
    ids = setting_ids()
    # the ini holds variant 0; every later variant sets every key any variant changes (undoing the previous one's)
    keyset = sorted({k for _, d in sets for k in d})
    ini_keys = dict(base, **sets[0][1])
    triples = [f"{k},{ids[key]},{int(dict(base, **d)[key])}" for k, (_, d) in enumerate(sets) if k for key in keyset]
    rt = runtime or rm.RT
    exe, ini = os.path.join(rt, "pw_bench12.exe"), os.path.join(rt, "ReShade.ini")
    argv = ([exe, str(frames)] + args + flags + ["--start-frame", str(begin), "--exposure-replay", track,
            "--texture-cache", rm.TEXTURE_CACHE, "--variants", str(len(names))] +
            (["--variant-set", ",".join(triples)] if triples else []) + list(extra) +
            ["--dump", ",".join(map(str, range(start, frames)))])
    cells = [os.path.join(out, view, rm.cell_name(n, 1, False, 1, short) + "_mv") for n in names]
    if len(set(cells)) != len(cells):
        raise ValueError("a run twice in one list")
    waited = rf.take_lock(f"motion run_variants.py {view} {','.join(names)}")
    saved = open(ini, "rb").read()
    try:
        old = os.path.join(out, "old", time.strftime("%Y%m%d-%H%M%S"))
        for cell in cells:
            if os.path.exists(cell):
                os.makedirs(os.path.join(old, view), exist_ok=True)
                shutil.move(cell, os.path.join(old, view, os.path.basename(cell)))
        if rf.move_all(rt, rm.OUTPUTS + ("v*_dump_*.png",), os.path.join(old, "stale_rt")):
            print(f"stale outputs in {rt} moved to {old}\\stale_rt")
        rf.set_ini(ini, ini_keys, [k for k in rm.UNSET if k not in ini_keys])
        os.makedirs(cells[0])
        t0 = time.time()
        with open(os.path.join(cells[0], "bench.out.txt"), "w") as so, open(os.path.join(cells[0], "bench.err.txt"), "w") as se:
            proc = subprocess.Popen(argv, cwd=rt, stdout=so, stderr=se, env=dict(os.environ, TEMP=rf.TMP, TMP=rf.TMP),
                                    creationflags=0x08000000)
            try:
                code = proc.wait(rf.TIMEOUT_S * len(names))
            except subprocess.TimeoutExpired:
                proc.kill()                                # only the PID this script started
                code = "timeout"
        wall = round(time.time() - t0, 1)
        evals = split_outputs(rt, cells, frames - begin)
        for cell in cells[1:]:
            os.makedirs(cell, exist_ok=True)
            for name in ("bench.out.txt", "bench.err.txt"):
                shutil.copy2(os.path.join(cells[0], name), cell)
        for cell in cells:
            shutil.copy2(ini, os.path.join(cell, "ReShade.ini"))
            if os.path.exists(os.path.join(rt, "ReShade.log")):
                shutil.copy2(os.path.join(rt, "ReShade.log"), cell)
    finally:
        open(ini, "wb").write(saved)
        os.remove(rf.LOCK)
    result = 0 if code == 0 else 1
    for k, (n, d) in enumerate(sets):
        meta = dict(view=view, run=n, cell=cells[k], variant=k, variants=names, set=d, ini=base, start_frame=begin,
                    dumps=[start, frames - 1], argv=argv, wall_s=wall, wall_per_variant_s=round(wall / len(names), 1),
                    exit=code, pid=proc.pid, lock_wait_s=waited, exe_sha256=rf.sha256(exe),
                    addon_sha256=rf.sha256(os.path.join(rt, "optimizer-fps-dlss5.addon64")),
                    core_sha256=rf.sha256(os.path.join(rt, "optimizer-fps-dlss5-core.dll")),
                    files=len(glob.glob(os.path.join(cells[k], "dump_*.png"))), eval_offset=evals[k])
        ofac = bool(dict(base, **d).get("DebugTemporalDumpAcc")) and dict(base, **d).get("TemporalMode") != "0"
        if ofac and code == 0:
            r0 = os.path.join(out, view, "R0")
            try:
                meta["ofac_check"] = check_run(cells[k], r0, rm.N, None, begin - evals[k])
            except ValueError as e:
                meta["ofac_check"], result = f"ABORT: {e}", 1
        json.dump(meta, open(os.path.join(cells[k], "meta.json"), "w"), indent=1)
        print(json.dumps({key: meta.get(key) for key in ("run", "cell", "exit", "wall_s", "files", "ofac_check")}))
        if not no_compact:
            rm.compact(cells[k])
    return result


def split_outputs(rt, cells, span):
    """Moves v<k>_dump_<f>.png to cell k as dump_<f>.png, and the OFAC files by their eval: each variant evaluates on
    frames S+1 .. frames-1 (span - 1 evals; feature 18 comes after frame S's present), the core numbers evals per process.
    -> each variant's eval offset (its evals minus the separate short run's)."""
    for k, cell in enumerate(cells):
        os.makedirs(cell, exist_ok=True)
        for f in glob.glob(os.path.join(rt, f"v{k}_dump_*.png")):
            shutil.move(f, os.path.join(cell, os.path.basename(f)[len(f"v{k}_"):]))
    offsets = [k * (span - 1) for k in range(len(cells))]
    for f in glob.glob(os.path.join(rt, "ofps_acc_*.bin")):
        ev = int(re.search(r"(\d+)\.bin$", f).group(1))
        k = min((ev - 1) // (span - 1), len(cells) - 1)
        shutil.move(f, os.path.join(cells[k], os.path.basename(f)))
    rf.move_all(rt, rm.OUTPUTS, os.path.join(cells[0], "unassigned"))
    return offsets


def main():
    rf.bench_paths.require_root()
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--view", choices=sorted(rm.VIEWS), required=True)
    ap.add_argument("--runs", required=True, help="comma-separated runs of run_motion.RUNS (T0, R0, ...)")
    ap.add_argument("--short", type=int, default=rm.WARMUP, metavar="W")
    ap.add_argument("--out", default=rm.OUT)
    ap.add_argument("--runtime")
    ap.add_argument("--bench-arg", action="append", default=[], metavar="ARG")
    ap.add_argument("--no-compact", action="store_true", help="leave the OFAC files uncompressed")
    a = ap.parse_args()
    try:
        return run_variants(a.view, a.runs.split(","), a.short, a.out, a.runtime, a.bench_arg, a.no_compact)
    except ValueError as e:
        ap.error(str(e))


if __name__ == "__main__":
    sys.exit(main())
