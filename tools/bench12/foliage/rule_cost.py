"""Cost of marker rules (phase 2) at 3840x2160, T1 N = 8, DebugTiming=1: runs of several rules interleaved under
bench.lock (rotating order per round). Samples the add-on's status rows (per-pass GPU times, model time) from the
shared mapping Local\\OptimizerFpsRemoteV4 (read-only, hosts/remote32/ipc.h layout) and the bench's GPU frame time.

Usage: python rule_cost.py <view> <config>... [--rounds 3] [--frames 600]
  view: held (garden, raster) or lab_follow (lab path traced, RR, follow camera)
  config: R (DebugTemporalMarkerRule) or R:K (with DebugTemporalMarkerNeighbours=K), e.g. 0 1 4:8
Writes <out>\\rules\\cost_<view>\\<round>_<config>.{status.jsonl,out.txt,err.txt,ini} and summary.json.
Pass times are the add-on's running means over the whole run (the last status sample); ModelMotion runs on full
passes only, so its row is the mean per full pass. "marker" = ModelMotion + MarkerFail (rules 3/4's first pass, also
once per full pass): the marker's whole price per full pass.
"""
import argparse, ctypes, json, os, re, statistics, struct, subprocess, sys, time
from ctypes import wintypes

import run_foliage as rf

MAPPING = "Local\\OptimizerFpsRemoteV4"
BLOCK_SIZE, STATUS_SEQ, STATUS_GEN, SNAP, SNAP_SIZE = 26504, 572, 576, 584, 25920   # ipc.h static_asserts
ROWCOUNT, ROWS, ROW_SIZE = 1152, 1156, 200
VIEWS = {"held": rf.VIEWS["held"] + ["--nr", "native"],
         "lab_follow": rf.LAB + ["--camera", "follow", "--move-speed", "7", "--nr", "native"]}
k32 = ctypes.WinDLL("kernel32", use_last_error=True)
k32.OpenFileMappingW.restype = wintypes.HANDLE
k32.OpenFileMappingW.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.LPCWSTR]
k32.MapViewOfFile.restype = ctypes.c_void_p
k32.MapViewOfFile.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.DWORD, wintypes.DWORD, ctypes.c_size_t]
k32.UnmapViewOfFile.argtypes = [ctypes.c_void_p]
k32.CloseHandle.argtypes = [wintypes.HANDLE]


def rows(addr):
    """{label: value} of the status rows, or None while the writer is mid-update."""
    before = ctypes.c_long.from_address(addr + STATUS_SEQ).value
    if before & 1:
        return None
    snap = ctypes.string_at(addr + SNAP, SNAP_SIZE)
    if ctypes.c_long.from_address(addr + STATUS_SEQ).value != before:
        return None
    cstr = lambda b: b.split(b"\0", 1)[0].decode("utf-8", "replace")
    n = min(struct.unpack_from("<I", snap, ROWCOUNT)[0], 32)
    return {cstr(snap[ROWS + i * ROW_SIZE + 8:ROWS + i * ROW_SIZE + 104]):
            cstr(snap[ROWS + i * ROW_SIZE + 104:ROWS + i * ROW_SIZE + 200]) for i in range(n)}


def config_keys(config):
    rule, _, k = config.partition(":")
    keys = {"DebugTemporalMarkerRule": rule} if rule != "0" else {}
    if k:
        keys["DebugTemporalMarkerNeighbours"] = k
    return keys


def one_run(view, config, frames, dest, name):
    keys = dict(rf.BASE, **rf.T1, **config_keys(config))
    ini = os.path.join(rf.RT, "ReShade.ini")
    rf.set_ini(ini, keys, [k for k in rf.DEBUG_KEYS if k not in keys])
    argv = [os.path.join(rf.RT, "pw_bench12.exe"), str(frames)] + VIEWS[view] + ["--width", "3840", "--height", "2160"]
    return dict(config=config, **sample(argv, rf.RT, dest, name))


def sample(argv, rt, dest, name):
    """Runs the bench in rt (its ReShade.ini already set) and samples the status rows -> exit, gpu_ms, samples, passes
    ({label: [running mean ms, count]}); writes <dest>\\<name>.{status.jsonl,out.txt,err.txt,ini}. chain_cost.py too."""
    ini = os.path.join(rt, "ReShade.ini")
    samples, addr, handle, last = [], None, None, None
    with open(os.path.join(dest, f"{name}.out.txt"), "w") as so, open(os.path.join(dest, f"{name}.err.txt"), "w") as se:
        proc = subprocess.Popen(argv, cwd=rt, stdout=so, stderr=se, env=dict(os.environ, TEMP=rf.TMP, TMP=rf.TMP),
                                creationflags=0x08000000)
        t0 = time.perf_counter()
        while proc.poll() is None:
            if addr is None and (handle := k32.OpenFileMappingW(4, False, MAPPING)):
                addr = k32.MapViewOfFile(handle, 4, 0, 0, BLOCK_SIZE) or None
            if addr is not None:
                gen = ctypes.c_uint32.from_address(addr + STATUS_GEN).value
                if gen != last and (r := rows(addr)) is not None:
                    samples.append(dict(t=round(time.perf_counter() - t0, 3), gen=gen, rows=r))
                    last = gen
            time.sleep(0.1)
    if addr:
        k32.UnmapViewOfFile(addr)
    if handle:
        k32.CloseHandle(handle)
    with open(os.path.join(dest, f"{name}.status.jsonl"), "w") as f:
        f.writelines(json.dumps(s) + "\n" for s in samples)
    with open(ini) as src, open(os.path.join(dest, f"{name}.ini"), "w") as dst:
        dst.write(src.read())
    text = open(os.path.join(dest, f"{name}.out.txt")).read()
    gpu = re.search(r"gpu frame time: avg ([\d.]+) ms", text)
    passes = {}
    for label, value in (samples[-1]["rows"].items() if samples else ()):
        if m := re.match(r"GPU ([\d.]+) ms \(n=(\d+)\)", value):
            passes[label] = [float(m[1]), int(m[2])]
        elif "Model GPU" in label and (m := re.match(r"([\d.]+) ms \(n=(\d+)\)", value)):
            passes["model"] = [float(m[1]), int(m[2])]
    if "ModelMotion" in passes:
        passes["marker"] = [passes["ModelMotion"][0] + passes.get("MarkerFail", [0.0])[0], passes["ModelMotion"][1]]
    return dict(exit=proc.returncode, gpu_ms=float(gpu[1]) if gpu else None, samples=len(samples), passes=passes,
                pid=proc.pid)


def main():
    rf.bench_paths.require_root()
    ap = argparse.ArgumentParser()
    ap.add_argument("view", choices=sorted(VIEWS))
    ap.add_argument("configs", nargs="+")
    ap.add_argument("--rounds", type=int, default=3)
    ap.add_argument("--frames", type=int, default=600)
    ap.add_argument("--out", default=rf.OUT)
    a = ap.parse_args()
    dest = os.path.join(a.out, "rules", f"cost_{a.view}")
    os.makedirs(dest, exist_ok=True)
    os.makedirs(rf.TMP, exist_ok=True)
    rf.take_lock(f"foliage rule_cost.py {a.view} {' '.join(a.configs)}")
    runs = []
    try:
        for k in range(a.rounds):
            order = a.configs[k % len(a.configs):] + a.configs[:k % len(a.configs)]   # rotate: no config always first
            for config in order:
                runs.append(dict(one_run(a.view, config, a.frames, dest, f"{k}_{config.replace(':', 'n')}"), round=k))
                print(json.dumps(runs[-1]), flush=True)
        rf.set_ini(os.path.join(rf.RT, "ReShade.ini"), rf.BASE, rf.DEBUG_KEYS)
    finally:
        os.remove(rf.LOCK)
    summary = {}
    for config in a.configs:
        mine = [r for r in runs if r["config"] == config]
        labels = sorted({k for r in mine for k in r["passes"]})
        summary[config] = dict(gpu_ms=[r["gpu_ms"] for r in mine],
                               passes={k: statistics.mean(r["passes"][k][0] for r in mine if k in r["passes"])
                                       for k in labels})
    json.dump(dict(view=a.view, frames=a.frames, runs=runs, summary=summary),
              open(os.path.join(dest, "summary.json"), "w"), indent=1)
    print(json.dumps(summary, indent=1))
    return 0 if all(r["exit"] == 0 for r in runs) else 1


if __name__ == "__main__":
    sys.exit(main())
