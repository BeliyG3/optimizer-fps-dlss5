"""The attribution table of phase 3 step 0 from every view's metrics.json (motion_metrics.py).

Usage: python motion_report.py [--root DIR] [--out FILE]   (default <root>\\attribution.md)
Causes (spec 0b, plan Task 4), each table on one stated base: cause 1 (the chain) = R1 - R0x; cause 3 (rejection,
fill) = R2 - R1; cause 2 (residual resampling) = R3a - R2 and R3b - R0; the expectation = R0 - R0x. Bases: the mean
PSNR of offsets 1-7 against T0 over the whole frame; the same off-band (outside the union of the cycle's disocclusion
bands, one mask for all offsets of a cycle); and the drop from offset 0 (mean of offsets 1-7 minus offset 0), which
leaves out what a probe changes on the full frame (the oracle also gives the model the exact motion there).
"""
import argparse, json, os, sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "foliage"))
import bench_paths                                         # noqa: E402

ROOT = bench_paths.out("motion")
VIEWS = ("combo", "pan", "held", "lab_combo", "lab_forward", "ghost_door", "lab_combo_raster", "lab_forward_raster")
RUNS = ("T0", "R0", "R0x", "R1", "R2", "R3a", "R3b")
NAN = float("nan")


def f(x, digits=2):
    return "-" if x is None or x != x else f"{x:.{digits}f}"


def table(head, rows):
    return "\n".join(["| " + " | ".join(head) + " |", "|" + "---|" * len(head)] +
                     ["| " + " | ".join(str(c) for c in r) + " |" for r in rows]) + "\n\n"


def cell(m, run, region, offset):
    v = m.get("m1", {}).get(run, {}).get(region, {}).get(str(offset))
    return v["psnr"] if v else NAN


def carried(m, run, region="whole"):
    """Mean PSNR over offsets 1-7 (dB average of the per-offset PSNRs)."""
    v = [cell(m, run, region, o) for o in range(1, m.get("n", 8))]
    v = [x for x in v if x == x]
    return sum(v) / len(v) if v else NAN


def drop(m, run, region="whole", offset=None):
    """Offset `offset` (default: the mean of 1-7) minus offset 0: what the carried frames lose against the full frame."""
    v = carried(m, run, region) if offset is None else cell(m, run, region, offset)
    return v - cell(m, run, region, 0)


def per_view(m):
    rows = [["T0", "n/a", "n/a", "n/a", "n/a", "n/a", "n/a", "n/a", "n/a", "reference"]]
    for r in RUNS[1:]:
        flick = m.get("m2", {}).get(r, {}).get("whole", {}).get("flicker")
        rows.append([r, f(cell(m, r, "whole", 0)), f(carried(m, r)), f(cell(m, r, "whole", 7)), f(drop(m, r)),
                     f(drop(m, r, offset=7)), f(cell(m, r, "whole_offband", 0)), f(carried(m, r, "whole_offband")),
                     f(drop(m, r, "whole_offband")), f(flick, 3)])
    return table(["run", "offset 0", "1-7 mean", "offset 7", "drop 1-7 vs 0", "drop 7 vs 0", "off-band 0",
                  "off-band 1-7", "off-band drop 1-7 vs 0", "M2 whole"], rows)


def causes(m, value):
    c = lambda r: value(m, r)
    return [f(c("R1") - c("R0x")), f(c("R2") - c("R1")), f(c("R3a") - c("R2")), f(c("R3b") - c("R0")), f(c("R0") - c("R0x"))]


def floor(m):
    """R2's loss on the carried frames against its own offset 0, on one region (whole, and the one off-band mask)."""
    return [f(carried(m, "R2")), f(cell(m, "R2", "whole", 0)), f(drop(m, "R2")), f(carried(m, "R2", "whole_offband")),
            f(drop(m, "R2", "whole_offband"))]


def band_rows(m):
    own, union = m.get("band_share", {}), m.get("band_union_share", {})
    return [[m["view"], m.get("band_rule") or "-"] + [f"{f(own.get(str(o)), 3)} / {f(union.get(str(o)), 3)}" for o in range(8)]]


def chain_rows(m):
    rows = []
    for r, regs in m.get("chain", {}).items():
        for reg in ("whole", "foliage", "rest"):
            for k, s in regs.get(reg, {}).items():
                rows.append([r, reg, k, f(s["coverage"], 3), f(s["median_err"]), f(s["p90_err"]), f(s["restart_share"], 3)])
    return rows


# ---- step 1: the chain-rule candidates against the owner's bar (plan 2026-10-09-camera-motion-carry-step1) --------
CANDIDATES = ("C1", "C2", "C3", "C4")
GAIN_VIEWS = ("combo", "pan")
IMAGE_VIEWS = ("combo", "pan", "held", "lab_combo_raster", "lab_forward_raster", "ghost_door")
BAR = {"m2_gain": 0.03, "m2_worse": 0.01, "offset0": 0.05, "mean17": 0.05, "offset7": 0.10, "band": 0.2, "mode3": 0.05,
       "cost_ms": 0.3}


def flick(m, run, region):
    return m.get("m2", {}).get(run, {}).get(region, {}).get("flicker", NAN)


def rel_m2(m, run, region):
    """Relative M2 change of run against R0 (positive = more flicker)."""
    return (flick(m, run, region) - flick(m, "R0", region)) / flick(m, "R0", region)


def evaluate(views, cand, cost=None):
    """Criteria 1-5 for one candidate against R0. views: {view: metrics.json}; cost: {config: chain ms per frame at 4K
    against R0} (chain_cost.py). -> {criterion: {"pass", "text", ...}}. A number that cannot be read fails."""
    have = {v: m for v, m in views.items() if cand in m.get("m1", {}) and "R0" in m.get("m1", {})}
    missing = [v for v in IMAGE_VIEWS if v not in have]
    gains = {v: -rel_m2(have[v], cand, "foliage") for v in GAIN_VIEWS if v in have}
    worse = [(rel_m2(m, cand, r), v, r) for v, m in have.items() for r in m.get("m2", {}).get("R0", {})]
    worse = max((w for w in worse if w[0] == w[0]), default=(NAN, "-", "-"))
    c1 = dict(gains=gains, worst=worse, text=f"foliage M2 {', '.join(f'{v} {-g * 100:+.1f} %' for v, g in gains.items())}; "
              f"worst {worse[0] * 100:+.1f} % ({worse[1]} {worse[2]})",
              ok=any(g >= BAR["m2_gain"] for g in gains.values()) and worse[0] <= BAR["m2_worse"])
    d = {v: (cell(m, cand, "whole", 0) - cell(m, "R0", "whole", 0), carried(m, cand) - carried(m, "R0"),
             cell(m, cand, "whole", 7) - cell(m, "R0", "whole", 7)) for v, m in have.items()}
    low = [min(((x[i], v) for v, x in d.items()), default=(NAN, "-")) for i in range(3)]
    c2 = dict(deltas=d, worst=low, text=f"worst 0 {low[0][0]:+.2f} ({low[0][1]}), 1-7 {low[1][0]:+.2f} ({low[1][1]}), "
              f"7 {low[2][0]:+.2f} ({low[2][1]})" + (f"; missing {', '.join(missing)}" if missing else ""),
              ok=not missing and low[0][0] >= -BAR["offset0"] and low[1][0] >= -BAR["mean17"] and low[2][0] >= -BAR["offset7"])
    g = have.get("ghost_door", {})
    b = (cell(g, cand, "band", 0) - cell(g, "R0", "band", 0), carried(g, cand, "band") - carried(g, "R0", "band")) if g else (NAN, NAN)
    c3 = dict(band=b, text=f"band 0 {b[0]:+.2f}, 1-7 {b[1]:+.2f}", ok=all(abs(x) <= BAR["band"] for x in b))
    base = views.get("combo", {}).get("mode3", {}).get("base", {})
    m3 = base.get(cand, {}).get("whole", NAN) - base.get("R0", {}).get("whole", NAN)
    c4 = dict(delta=m3, text=f"whole {m3:+.2f}", ok=m3 >= -BAR["mode3"])
    ms = (cost or {}).get(cand)
    c5 = dict(delta=ms, text="not measured" if ms is None else f"{ms:+.3f} ms", ok=ms is not None and ms <= BAR["cost_ms"])
    return {1: c1, 2: c2, 3: c3, 4: c4, 5: c5}


def pick(views, evals):
    """The winner: passes 1-5, the largest foliage M2 gain on combo + pan (mean of the relative gains), ties (0.1 %)
    to the higher whole-frame mean of offsets 1-7 (mean over combo and pan). None if no candidate passes."""
    ok = [c for c, e in evals.items() if all(e[k]["ok"] for k in range(1, 6))]
    key = lambda c: (round(sum(evals[c][1]["gains"].values()) / len(GAIN_VIEWS), 3),
                     sum(evals[c][2]["deltas"][v][1] for v in GAIN_VIEWS) / len(GAIN_VIEWS))
    return max(ok, key=key) if ok else None


CARRY = ("S1", "S2", "S3", "S4")          # step 1b: DebugTemporalCarryChainRule 1-4 (two chains)
CARRY_1C = ("S3", "S5", "S6")             # step 1c: rules 5/6 (coherent change) and S3 again, merged chain passes
COST = {CANDIDATES: "cost_combo", CARRY: "cost_combo_step1b", CARRY_1C: "cost_combo_step2c"}   # chain_cost.py --name


def model_frames(views, cand):
    """Criterion 6 for a carry chain: the model's frames (offsets 0 and 1) byte-identical to R0's on every view
    (offset_identity of motion_metrics.py). Identity at rule 0 is the build's and is checked separately (step1c_identity)."""
    got = {v: views[v].get("offset_identity", {}).get(cand) for v in IMAGE_VIEWS if v in views}
    bad = [v for v, o in got.items() if not o or any(o.get(k, [0, 1])[0] != o.get(k, [0, 1])[1] for k in ("0", "1"))]
    return dict(text=f"offsets 0 and 1 identical to R0 on {len(got) - len(bad)}/{len(IMAGE_VIEWS)} views"
                + (f" (not: {', '.join(bad)})" if bad else ""), ok=not bad and len(got) == len(IMAGE_VIEWS))


def candidates_section(found, root, cands=CANDIDATES, title="Step 1: chain-rule candidates"):
    path = os.path.join(root, COST[cands], "summary.json")
    cost = {c: s.get("delta_chain_ms") for c, s in json.load(open(path))["summary"].items()} if os.path.exists(path) else {}
    evals = {c: evaluate(found, c, cost) for c in cands if any(c in m.get("m1", {}) for m in found.values())}
    if not evals:
        return ""
    if cands == CARRY_1C:
        for c in evals:
            evals[c][6] = model_frames(found, c)
    ks = range(1, 7 if cands == CARRY_1C else 6)
    pf = lambda e: ("pass" if e["ok"] else "FAIL") + ": " + e["text"]
    out = [f"## {title} against R0 (criteria {ks[0]}-{ks[-1]})\n\n",
           table(["candidate"] + [f"criterion {k}" for k in ks], [[c] + [pf(e[k]) for k in ks] for c, e in evals.items()])]
    win = pick(found, evals)
    out.append(f"Winner: **{win or 'none'}**.\n\n")
    rows = [[v, c, f(cell(m, c, "whole", 0) - cell(m, "R0", "whole", 0)), f(carried(m, c) - carried(m, "R0")),
             f(cell(m, c, "whole", 7) - cell(m, "R0", "whole", 7))] +
            [f(100 * rel_m2(m, c, r), 1) for r in ("foliage", "edge", "rest", "whole")]
            for v, m in found.items() for c in cands if c in m.get("m1", {})]
    out.append(table(["view", "run", "d offset 0", "d 1-7", "d offset 7", "M2 foliage %", "M2 edge %", "M2 rest %",
                      "M2 whole %"], rows))
    stat = lambda m, r, k, key: m.get("chain", {}).get(r, {}).get("foliage", {}).get(str(k), {}).get(key)
    out.append(table(["view", "run", "foliage coverage 3 / 4 / 7", "foliage restart 2 / 4 / 7"],
                     [[v, r, " / ".join(f(stat(m, r, k, "coverage")) for k in (3, 4, 7)),
                       " / ".join(f(stat(m, r, k, "restart_share"), 3) for k in (2, 4, 7))]
                      for v, m in found.items() for r in ("R0_raw",) + tuple(c + "_raw" for c in cands)
                      if r in m.get("chain", {})]))
    return "".join(out)


def report(root):
    found = {v: json.load(open(os.path.join(root, v, "metrics.json"))) for v in VIEWS
             if os.path.exists(os.path.join(root, v, "metrics.json"))}
    out = ["# Camera-motion carry, step 0: attribution\n\n",
           f"Source: `{root}`; PSNR in dB against T0 (whole frame unless named); offsets since the last full pass, "
           "N = 8; frames before the second full pass excluded. Off-band = outside the union of the disocclusion bands "
           "(and off-screen sources) of the frame's cycle, from R1_raw and the prevdepth run (band rule below).\n\n"]
    for v, m in found.items():
        out.append(f"## {v}\n\nP = {m.get('phase')}, frames {m.get('frames')}, runs {', '.join(m['runs'])}.\n\n")
        out.append(per_view(m) if "m1" in m else "No mode-1 metrics (T0 or R0 missing).\n\n")
    head = ["cause 1: R1 - R0x", "cause 3: R2 - R1", "cause 2: R3a - R2", "cause 2: R3b - R0", "expectation: R0 - R0x"]
    bases = (("whole frame, mean of offsets 1-7", lambda m, r: carried(m, r)),
             ("off-band (one mask per cycle), mean of offsets 1-7", lambda m, r: carried(m, r, "whole_offband")),
             ("whole frame, drop of offsets 1-7 from offset 0", lambda m, r: drop(m, r)))
    for name, value in bases:
        out.append(f"## Recovered dB per cause, base: {name}\n\n")
        out.append(table(["view"] + head, [[v] + causes(m, value) for v, m in found.items() if "m1" in m]))
    out.append("## Floor (cause 5)\n\nWhat R2 still loses on the carried frames, against T0 and against its own offset 0, "
               "each on one region (the whole frame; the off-band mask, the same pixels for every offset of a cycle).\n\n")
    out.append(table(["view", "R2 1-7 whole", "R2 offset 0 whole", "drop 1-7 vs 0", "R2 1-7 off-band", "off-band drop 1-7 vs 0"],
                     [[v] + floor(m) for v, m in found.items() if "m1" in m]))
    out.append("## Disocclusion band by offset (own band / the cycle's union; share of the frame)\n\n")
    out.append(table(["view", "rule"] + [str(o) for o in range(8)], [r for m in found.values() if "m1" in m for r in band_rows(m)]))
    out.append("## Inside the band (the cycle's union; the ghost guard's region)\n\n")
    out.append(table(["view", "run", "offset 0", "1-7 mean", "offset 7"],
                     [[v, r, f(cell(m, r, "band", 0)), f(carried(m, r, "band")), f(cell(m, r, "band", 7))]
                      for v, m in found.items() for r in RUNS[1:] if "band" in m.get("m1", {}).get(r, {})]))
    out.append("## Off-screen sources (cause 4), carried frames\n\n")
    out.append(table(["view", "run", "pixel share", "loss share", "PSNR inside", "PSNR off-screen"],
                     [[v, r, f(s["share_out"], 3), f(s["loss_share_out"], 3), f(s["psnr_in"]), f(s["psnr_out"])]
                      for v, m in found.items() for r, s in m.get("offscreen", {}).items()]))
    out.append("## Chain against the true offset (motion-texture px)\n\nCoverage = median |chain| / |true| over |true| > 2 px; "
               "restart = |chain - this frame's vector| < 0.05 px where |true - vector| > 1 px (offsets > 1). "
               "R1_raw reads 1 / 0 / 0 by construction (its chain is the bench's offset) and its restart share is always '-' "
               "(its own vector is the true offset, so no restart is detectable). d = 0 is checked on R0_raw: its median "
               "error on offsets 2-3 (rest) must stay below 0.25 px (Checks).\n\n")
    for v, m in found.items():
        rows = chain_rows(m)
        if rows:
            out.append(f"### {v}\n\n" + table(["run", "region", "offset", "coverage", "median err", "p90 err", "restart"], rows))
    out.append("## M2 flicker (mean |e_t - e_t-1| of the luma error; 16 px tiles in brackets)\n\n")
    out.append(table(["view", "run", "foliage", "edge", "rest", "whole"],
                     [[v, r] + [f"{f(x.get(g, {}).get('flicker'), 3)} ({f(x.get(g, {}).get('flicker_low'), 3)})"
                                for g in ("foliage", "edge", "rest", "whole")]
                      for v, m in found.items() for r, x in m.get("m2", {}).items()]))
    out.append("## TemporalMode 3 (rt_motion_bg, against T0 of the same runtime; R0 over its repeats)\n\n")
    out.append(table(["view", "R0 runs", "whole mean", "whole spread", "foliage mean", "rest mean"],
                     [[v, sum(r.startswith("R0_") for r in m["mode3"]["runs"]), f(m["mode3"]["mean"].get("whole")),
                       f(m["mode3"]["spread"].get("whole")), f(m["mode3"]["mean"].get("foliage")), f(m["mode3"]["mean"].get("rest"))]
                      for v, m in found.items() if m.get("mode3")]))
    out.append(table(["view", "run", "whole", "foliage", "rest"],
                     [[v, r, f(x.get("whole")), f(x.get("foliage")), f(x.get("rest"))]
                      for v, m in found.items() for r, x in m.get("mode3", {}).get("runs", {}).items()]))
    out.append(candidates_section(found, root))
    out.append(candidates_section(found, root, CARRY, "Step 1b: two chains (S = carry chain rule, model rule 0)"))
    out.append(candidates_section(found, root, CARRY_1C, "Step 1c: coherent change, merged chain passes"))
    ident = [[v, r] + [f"{x[0]}/{x[1]}" if (x := o.get(str(k))) else "-" for k in range(m.get("n", 8))]
             for v, m in found.items() for r, o in m.get("offset_identity", {}).items()]
    if ident:
        out.append("Step 1b: dumps byte-identical to R0 (S<n>_raw to R0_raw) by offset; the model's frames (0, 1) must be.\n\n")
        out.append(table(["view", "run"] + [f"offset {k}" for k in range(8)], ident))
    out.append("## Checks\n\n")
    out.append(table(["view", "run", "OFAC evals (first, last, count)", "missing"],
                     [[v, r, o["evals"], f"{o['missing']} {o['missing_evals'][:8] if o['missing'] else ''}"]
                      for v, m in found.items() for r, o in m.get("ofac", {}).items()]))
    out.append(table(["view", "d check: R0_raw median error, offsets 2-3", "median abs(g), px", "region", "ok"],
                     [[v, f(d["median_err_k2_3"], 3), f(d.get("median_g_k2_3"), 1), d["region"],
                       d["ok"] if d.get("valid", True) else "invalid (median abs(g) < 2 px)"]
                      for v, m in found.items() for d in [m.get("d_check")] if d]))
    rep = [[v, r, x["of"], f"{x['identical']}/{x['shared']}", x["byte_identical"]]
           for v, m in found.items() for r, x in m.get("repeats", {}).items()]
    if rep:
        out.append(table(["view", "repeat", "of", "identical files", "byte-identical"], rep))
    notes = [f"- {v}: {n}" for v, m in found.items() for n in m.get("notes", [])]
    out.append("Notes:\n\n" + ("\n".join(notes) if notes else "- none") + "\n")
    return "".join(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=ROOT, required=ROOT is None)
    ap.add_argument("--out")
    a = ap.parse_args()
    path = a.out or os.path.join(a.root, "attribution.md")
    open(path, "w", encoding="utf-8").write(report(a.root))
    print(path)


if __name__ == "__main__":
    main()
