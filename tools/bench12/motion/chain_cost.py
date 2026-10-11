"""GPU cost of the chain rules (phase 3 step 1, success criterion 5) at 3840x2160, TemporalMode 1, N = 8,
DebugTiming=1: R0 and the candidates interleaved under bench.lock in rt_motion, the order rotated per round. Sampling
is foliage/rule_cost.py's (the add-on's status rows from the shared mapping, the bench's GPU frame time).

Usage: python chain_cost.py [config ...] [--view combo] [--rounds 3] [--frames 600]
  config: R0, C1..C4 or S1..S6 (default: R0 and C1..C4)
Writes <out>\\<name, default cost_<view>>\\<round>_<config>.{status.jsonl,out.txt,err.txt,ini} and summary.json. Per
config and round, per frame = over Expect's count (once per frame in every config):
  chain = Accumulate + Refine + LinkFail + their merged pair rows (the passes a rule changes; step 1b's carry chain ran
  Accumulate and Refine a second time per frame under the same timer rows, step 1c's runs AccumulatePair, RefinePair and
  LinkFailPair instead on the carried frames, step 1d's AccumulatePair and RefinePair alone); temporal = every pass but
  the model;
  gpu_ms = the bench's average frame time. Task 2's cost_combo (C1-C4) was read before Refine was counted. summary[config]["delta_*"] = the mean over rounds of
  the difference to R0 of the same round.
"""
import argparse, json, os, statistics, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "foliage"))
import run_foliage as rf                                   # noqa: E402
from rule_cost import sample                               # noqa: E402
import run_motion as rm                                    # noqa: E402

CONFIGS = ("R0",) + rm.CANDIDATES + rm.CARRY_CANDIDATES
SKIP = ("model", "marker")                                 # model: the same in every config; marker: a sum of two rows
# step 1c (core d089024): with a carry chain the carried frames run the merged AccumulatePair / RefinePair and, under the
# voting rules (3, 5, 6), LinkFailPair (gone in step 1d: AccumulatePair votes itself); full passes keep the single
# Accumulate and Refine
CHAIN = ("Accumulate", "Refine", "LinkFail", "AccumulatePair", "RefinePair", "LinkFailPair")


def per_frame(passes):
    frames = passes.get("Expect") or passes.get("Accumulate")
    if not frames or not frames[1]:
        return None, None
    scale = lambda k: passes[k][0] * passes[k][1] / frames[1] if k in passes else 0.0
    return sum(scale(k) for k in CHAIN), sum(scale(k) for k in passes if k not in SKIP)


def one_run(view, config, frames, dest, name):
    keys = dict(rm.BASE, **rm.RUNS[config][0])
    keys.pop("DebugTemporalDumpAcc", None)                 # no OFAC dumps while timing
    ini = os.path.join(rm.RT, "ReShade.ini")
    rf.set_ini(ini, keys, [k for k in rm.UNSET if k not in keys])
    argv = [os.path.join(rm.RT, "pw_bench12.exe"), str(frames)] + rm.VIEWS[view][0] + \
        ["--nr", "native", "--width", "3840", "--height", "2160"]
    r = sample(argv, rm.RT, dest, name)
    chain, temporal = per_frame(r["passes"])
    return dict(config=config, chain_ms=chain, temporal_ms=temporal, **r)


def summarize(runs):
    out = {}
    for c in dict.fromkeys(r["config"] for r in runs):
        mine = {r["round"]: r for r in runs if r["config"] == c}
        base = {r["round"]: r for r in runs if r["config"] == "R0"}
        s = {k: [mine[i][k] for i in sorted(mine)] for k in ("chain_ms", "temporal_ms", "gpu_ms")}
        for k in list(s):
            d = [mine[i][k] - base[i][k] for i in mine if i in base and mine[i][k] is not None and base[i][k] is not None]
            s[f"delta_{k}"] = statistics.mean(d) if d else None
            s[f"delta_{k}_rounds"] = d
        out[c] = s
    return out


def main():
    rf.bench_paths.require_root()
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("configs", nargs="*", default=list(CONFIGS[:5]))
    ap.add_argument("--name", help="output folder under --out (default cost_<view>)")
    ap.add_argument("--view", default="combo", choices=sorted(rm.VIEWS))
    ap.add_argument("--rounds", type=int, default=3)
    ap.add_argument("--frames", type=int, default=600)
    ap.add_argument("--out", default=rm.OUT)
    a = ap.parse_args()
    if "R0" not in a.configs or any(c not in CONFIGS for c in a.configs):
        ap.error(f"configs from {CONFIGS}, R0 included (the deltas are per round against it)")
    dest = os.path.join(a.out, a.name or f"cost_{a.view}")
    os.makedirs(dest, exist_ok=True)
    os.makedirs(rf.TMP, exist_ok=True)
    rf.take_lock(f"motion chain_cost.py {a.view} {' '.join(a.configs)}")
    runs = []
    try:
        for k in range(a.rounds):
            order = a.configs[k % len(a.configs):] + a.configs[:k % len(a.configs)]   # rotate: no config always first
            for c in order:
                runs.append(dict(one_run(a.view, c, a.frames, dest, f"{k}_{c}"), round=k))
                print(json.dumps({x: runs[-1][x] for x in ("round", "config", "exit", "chain_ms", "temporal_ms", "gpu_ms")}),
                      flush=True)
        rf.set_ini(os.path.join(rm.RT, "ReShade.ini"), {}, rm.UNSET)
    finally:
        os.remove(rf.LOCK)
    summary = summarize(runs)
    json.dump(dict(view=a.view, frames=a.frames, size=[3840, 2160], runs=runs, summary=summary),
              open(os.path.join(dest, "summary.json"), "w"), indent=1)
    print(json.dumps({c: {k: v for k, v in s.items() if k.startswith("delta") and not k.endswith("rounds")}
                      for c, s in summary.items()}, indent=1))
    return 0 if all(r["exit"] == 0 for r in runs) else 1


if __name__ == "__main__":
    sys.exit(main())
