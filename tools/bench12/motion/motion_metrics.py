"""Metrics of the camera-motion carry attribution (phase 3 step 0, motion/README.md) for one view.

Usage: python motion_metrics.py <view> [--root DIR] [--mask DIR]
Reads the run folders run_motion.py wrote under <root>\\<view>\\ and writes <root>\\<view>\\metrics.json:
  phase, warmup_end, frames; ofac[run] (evals, missing evals: the dump window can drop frames);
  m1[run][region][offset] (psnr, mad) against T0; m2[run][region] (flicker, flicker_low);
  offscreen[run] (carried frames: psnr in/out, pixel and loss share of off-screen-sourced pixels);
  chain[run][region][offset] (coverage, median_err, p90_err, restart_share) for R0_raw and R1_raw;
  mode3 (R0_m3 repeats against T0_m3), repeats (byte identity of <run>_r<k> against <run>), notes.
Regions: foliage_metrics.regions of the view's vegmask (the `mask` run, else phase 1's out\\foliage\\<view>\\mask\\r1),
plus `whole`, and `whole_offband`: outside the union of the disocclusion bands of the frame's cycle (motion_band.py),
where R1_raw's raw dumps allow it; band_rule says whether the band is exact (prevdepth run) or approximate, band_share and
band_union_share give the share of the frame in the own band and in the cycle's union by offset. d_check: R0_raw's chain
against the true offset on offsets 2-3 (a wrong eval -> frame offset d would leave errors of pixels there).
"""
import argparse, json, os, re, struct, sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "foliage"))
from f16io import read_ofac                                                        # noqa: E402
from foliage_metrics import (NAN, by_offset, classes, flicker, flicker_low, frames_of, gt_motion, luma, psnr,  # noqa: E402
                             regions, rgb)
from ghost_metrics import resize_nearest                                           # noqa: E402
from repeats import digest, files                                                  # noqa: E402

from motion_band import TRUTH, disocclusion_band, frame_fields, offscreen_mask, raw_fields, unpack  # noqa: E402, F401
from motion_ofac import check_run, ofac_heads, phase_from_evals                    # noqa: E402, F401

import bench_paths                                                                # noqa: E402

ROOT = bench_paths.out("motion")
N = 8
CANDIDATES = ("C1", "C2", "C3", "C4")                       # step 1: R0 + DebugTemporalChainRule 1-4
CARRY = ("S1", "S2", "S3", "S4", "S5", "S6")                # step 1b/1c: R0 + DebugTemporalCarryChainRule 1-6
M1_RUNS = ("R0", "R0x", "R1", "R2", "R3a", "R3b") + CANDIDATES + CARRY
# S<n>_raw: an OFAC v3 dump (core d089024 on) carries the carry chain as a fourth plane, and the chain statistics read it
# (chain_plane[run] = "carry"); a v2 dump (step 1b) holds only the model's chain, which equals R0_raw's ("model")
CHAIN_RUNS = ("R0_raw", "R1_raw") + tuple(c + "_raw" for c in CANDIDATES + CARRY)
M2_REGIONS = ("foliage", "edge", "rest", "whole")
CHAIN_REGIONS = ("foliage", "rest")
D_CHECK_PX = 0.25          # R0_raw's median chain error on offsets 2-3 (rest, else whole) must stay below this


# ---- primitives (test_motion_metrics.py) ---------------------------------------------------------------------
def offset_of(frame, phase, n=N):
    return (frame - phase) % n


def warmup_end(phase, n=N):
    """The second full pass: earlier frames are excluded (the oracle offsets clamp to frame 0 before it)."""
    return phase + n


def full_cycles(frames, phase, n=N):
    """The frames whose whole cycle (P + n c .. P + n c + n - 1) is present: every offset on the same cycles."""
    count = {}
    for f in frames:
        count[(f - phase) // n] = count.get((f - phase) // n, 0) + 1
    return [f for f in frames if count[(f - phase) // n] == n]


def chain_stats(a, g, mv, mask=None):
    """Chain a against the true offset g, mv this frame's own vector (all motion-texture px [h, w, 2]).
    coverage = median |a| / |g| over |g| > 2 px; err = |a - g|; restart (planning ruling): |a - mv| < 0.05 px among
    the pixels where a restart is detectable, |g - mv| > 1 px; median_g = median |g|. NaN where a statistic has no pixel."""
    m = np.isfinite(a).all(-1) & np.isfinite(g).all(-1)
    if mask is not None:
        m &= mask
    a, g, mv = a[m].astype(np.float64), g[m].astype(np.float64), mv[m].astype(np.float64)
    norm = lambda v: np.hypot(v[:, 0], v[:, 1])
    lg, err, det = norm(g), norm(a - g), norm(g - mv) > 1
    big = lg > 2
    return {"coverage": float(np.median(norm(a[big]) / lg[big])) if big.any() else NAN,
            "median_err": float(np.median(err)) if err.size else NAN,
            "p90_err": float(np.percentile(err, 90)) if err.size else NAN, "median_g": float(np.median(lg)) if lg.size else NAN,
            "restart_share": float(np.mean(norm(a[det] - mv[det]) < 0.05)) if det.any() else NAN}


# ---- one view --------------------------------------------------------------------------------------------------
def discover(view_dir):
    return {name: os.path.join(view_dir, name) for name in sorted(os.listdir(view_dir))
            if os.path.isfile(os.path.join(view_dir, name, "meta.json"))}


def meta_of(d):
    return json.load(open(os.path.join(d, "meta.json")))


def find_mask(view_dir):
    phase1 = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(view_dir))), "foliage")   # out\foliage
    for d in (os.path.join(view_dir, "mask"), os.path.join(phase1, os.path.basename(view_dir), "mask", "r1")):
        if os.path.isdir(d):
            return d
    return None


def mean_of(values):
    v = [x for x in values if x == x]
    return float(np.mean(v)) if v else NAN


def summarize(vals, phase):
    """{frame: (mse, mad)} -> {offset: {psnr (from the mean MSE), mad}}."""
    mse, mad = (by_offset({f: x[i] for f, x in vals.items()}, phase, N) for i in (0, 1))
    return {o: {"psnr": psnr(v), "mad": mad[o]} for o, v in mse.items()}


def colour_metrics(runs, phase, frames, mask, fields, notes):
    """M1, M2 and the off-screen split; fields = motion_band.frame_fields (None: no band, no split)."""
    m1_runs = [r for r in M1_RUNS if r in runs]
    per = {r: {} for r in m1_runs}                         # run -> region -> frame -> (mse, mad)
    flick = {r: {} for r in m1_runs}                       # run -> region -> [(flicker, flicker_low)]
    offs = {r: np.zeros(4) for r in m1_runs}               # carried frames: SSE and values inside, then off-screen
    prev = {}
    for f in frames:
        t0 = rgb(runs["T0"], f).astype(np.float64)
        shape = t0.shape[:2]
        regs = {"whole": np.ones(shape, bool)}
        if mask and os.path.exists(os.path.join(mask, f"dump_{f}.png")):
            regs.update(regions(classes(mask, f)))
        off = None
        if fields and f in fields["off"]:
            raw_off = unpack(fields["off"][f])
            if raw_off.shape != shape:
                notes.add(f"raw fields {raw_off.shape} resized (nearest) to the colour dumps {shape}")
            off = resize_nearest(raw_off, shape)
            regs["whole_offband"] = ~resize_nearest(fields["union"][(f - phase) // N], shape)
            regs["band"] = ~regs["whole_offband"]         # the cycle's union: the ghost guard's region
        lt0 = luma(t0)
        for r in m1_runs:
            img = rgb(runs[r], f).astype(np.float64)
            diff = img - t0
            for reg, m in regs.items():
                d = diff[m]
                per[r].setdefault(reg, {})[f] = (float(np.mean(d ** 2)), float(np.mean(np.abs(d)))) if d.size else (NAN, NAN)
            e = luma(img) - lt0
            if r in prev and prev[r][0] == f - 1:
                for reg in M2_REGIONS:
                    if reg in regs:
                        pair = [prev[r][1], e]
                        flick[r].setdefault(reg, []).append((flicker(pair, regs[reg]), flicker_low(pair, regs[reg])))
            prev[r] = (f, e)
            if off is not None and offset_of(f, phase):
                sq = np.sum(diff ** 2, axis=-1)
                offs[r] += [sq[~off].sum(), 3 * np.count_nonzero(~off), sq[off].sum(), 3 * np.count_nonzero(off)]
    m1 = {r: {reg: summarize(vals, phase) for reg, vals in per[r].items()} for r in m1_runs}
    m2 = {r: {reg: {"flicker": mean_of([a for a, _ in v]), "flicker_low": mean_of([b for _, b in v])}
              for reg, v in flick[r].items()} for r in m1_runs}
    offscreen = {}
    for r, (s_in, n_in, s_out, n_out) in offs.items():
        if n_in + n_out:
            offscreen[r] = {"psnr_in": psnr(s_in / n_in) if n_in else NAN, "psnr_out": psnr(s_out / n_out) if n_out else NAN,
                            "share_out": n_out / (n_in + n_out), "loss_share_out": s_out / (s_in + s_out) if s_in + s_out else NAN}
    return m1, m2, offscreen


def chain_metrics(runs, phase, start, mask, notes, planes=None):
    out, truth = {}, runs.get(TRUTH)
    planes = {} if planes is None else planes
    for r in (r for r in CHAIN_RUNS if r in runs and truth):
        per = {}
        for ev in sorted(ofac_heads(runs[r])):
            k = offset_of(ev, phase)                      # frame = eval (d = 0)
            mv_path = os.path.join(runs[r], f"motion_{ev}.f32")
            if not k or ev < start or not os.path.exists(mv_path) or not os.path.exists(os.path.join(truth, f"motion_{ev}.f32")):
                continue
            _, _, a, _, carry = read_ofac(os.path.join(runs[r], f"ofps_acc_{ev}.bin"), with_carry=True)
            if r.startswith("S") and carry is not None:
                a, planes[r] = carry, "carry"
            else:
                planes.setdefault(r, "model")
            g, mv = gt_motion(truth, ev, a.shape[:2], notes), gt_motion(runs[r], ev, a.shape[:2], notes)
            regs = {"whole": None}
            if mask and os.path.exists(os.path.join(mask, f"dump_{ev}.png")):
                rg = regions(classes(mask, ev))
                regs.update({reg: resize_nearest(rg[reg], a.shape[:2]) for reg in CHAIN_REGIONS})
            for reg, m in regs.items():
                s = chain_stats(a, g, mv, m)
                if k == 1:
                    s["restart_share"] = NAN              # a one-frame chain equals this frame's vector anyway
                per.setdefault(reg, {}).setdefault(k, []).append(s)
        out[r] = {reg: {k: {key: mean_of([s[key] for s in v]) for key in v[0]} for k, v in sorted(ks.items())}
                  for reg, ks in per.items()}
    return out


def mode3_metrics(runs, mask):
    """Every <run>_m3[_r<k>] against T0_m3 over all dumped frames. mean/spread: R0_m3's repeats (step 0's numbers);
    base[run][region]: the mean over that run's repeats (step 1's C<n>_m3 and step 1b's S<n>_m3 against R0)."""
    reps = [r for r in runs if re.fullmatch(r"(R0|C\d|S\d)_m3(_r\d+)?", r)]
    if "T0_m3" not in runs or not reps:
        return {}
    frames = [f for f in frames_of(runs["T0_m3"]) if all(os.path.exists(os.path.join(runs[r], f"dump_{f}.png")) for r in reps)]
    sums = {r: {} for r in reps}                          # run -> region -> [SSE, values]
    for f in frames:
        t0 = rgb(runs["T0_m3"], f).astype(np.float64)
        regs = {"whole": np.ones(t0.shape[:2], bool)}
        if mask and os.path.exists(os.path.join(mask, f"dump_{f}.png")):
            regs.update({k: v for k, v in regions(classes(mask, f)).items() if k in ("foliage", "rest")})
        for r in reps:
            diff = rgb(runs[r], f) - t0
            for reg, m in regs.items():
                s = sums[r].setdefault(reg, [0.0, 0])
                s[0] += float(np.sum(diff[m] ** 2))
                s[1] += diff[m].size
    res = {r: {reg: psnr(s / n) for reg, (s, n) in v.items() if n} for r, v in sums.items()}
    groups = {}
    for r in reps:
        groups.setdefault(r.split("_m3")[0], []).append(r)
    base = {b: {g: float(np.mean([res[r][g] for r in rs])) for g in set.intersection(*(set(res[r]) for r in rs))}
            for b, rs in groups.items()}
    r0 = groups.get("R0", [])
    regs = sorted(set.intersection(*(set(res[r]) for r in r0))) if r0 else []
    return {"frames": [frames[0], frames[-1], len(frames)] if frames else [], "runs": res, "base": base,
            "mean": {g: float(np.mean([res[r][g] for r in r0])) for g in regs},
            "spread": {g: float(np.ptp([res[r][g] for r in r0])) for g in regs}}


def repeat_identity(runs):
    out = {}
    for r, d in runs.items():
        m = re.fullmatch(r"(.+)_r(\d+)", r)
        if m and m.group(1) in runs:
            a = runs[m.group(1)]
            shared = sorted(set(files(a)) & set(files(d)))
            same = sum(digest(os.path.join(a, f)) == digest(os.path.join(d, f)) for f in shared)
            out[r] = {"of": m.group(1), "identical": same, "shared": len(shared), "byte_identical": bool(shared) and same == len(shared)}
    return out


def offset_identity(runs, phase, n=N):
    """Step 1b: S<n> must leave the model's frames alone. Per S run (and S<n>_raw against R0_raw), per offset: [PNG dumps
    byte-identical to R0's, dumps compared]. Expected: offsets 0 and 1 all identical (the full pass and its first link)."""
    out = {}
    for r in runs:
        base = "R0_raw" if r.endswith("_raw") else "R0"
        if not re.fullmatch(r"S\d(_raw)?", r) or base not in runs:
            continue
        per = {}
        for f in frames_of(runs[r]):
            a = os.path.join(runs[base], f"dump_{f}.png")
            if os.path.exists(a):
                c = per.setdefault(offset_of(f, phase, n), [0, 0])
                c[0] += digest(a) == digest(os.path.join(runs[r], f"dump_{f}.png"))
                c[1] += 1
        out[r] = {o: per[o] for o in sorted(per)}
    return out


def view_metrics(view_dir, mask=None):
    notes = set()
    runs = discover(view_dir)
    mask = mask or find_mask(view_dir)
    if not mask:
        notes.add("no vegmask: the whole frame only")
    res = {"view": os.path.basename(view_dir), "n": N, "runs": sorted(runs), "mask": mask, "ofac": {},
           "mode3": mode3_metrics(runs, mask), "repeats": repeat_identity(runs)}
    if "R0" not in runs or "T0" not in runs:
        notes.add("T0 or R0 missing: no mode-1 metrics")
        return dict(res, notes=sorted(notes))
    for r, d in runs.items():
        meta = meta_of(d)
        if r != "R0" and meta.get("mode") == 1 and meta.get("ofac"):
            res["ofac"][r] = check_run(d, runs["R0"], N, meta.get("acc_window"))
    res["ofac"]["R0"] = check_run(runs["R0"], runs["R0"], N)
    phase = res["ofac"]["R0"]["phase"]
    start = warmup_end(phase)
    m1_runs = [r for r in M1_RUNS if r in runs]
    frames = full_cycles([f for f in frames_of(runs["T0"]) if f >= start and
                          all(os.path.exists(os.path.join(runs[r], f"dump_{f}.png")) for r in m1_runs)], phase)
    fields = frame_fields(runs, phase, frames, notes)
    m1, m2, offscreen = colour_metrics(runs, phase, frames, mask, fields, notes)
    if TRUTH not in runs:
        notes.add(f"{TRUTH} missing: no off-screen split, no disocclusion band (whole_offband), no chain statistics")
    planes = {}
    chain = chain_metrics(runs, phase, start, mask, notes, planes)
    band = {k: fields[k] for k in ("rule", "band_share", "union_share")} if fields else {}
    return dict(res, phase=phase, warmup_end=start, frames=[frames[0], frames[-1], len(frames)] if frames else [],
                m1=m1, m2=m2, offscreen=offscreen, band_rule=band.get("rule"), band_share=band.get("band_share", {}),
                band_union_share=band.get("union_share", {}), chain=chain, chain_plane=planes, d_check=d_check(chain),
                offset_identity=offset_identity(runs, phase), notes=sorted(notes))


def d_check(chain):
    """R0_raw's chain must match the true offset on offsets 2-3 where little restarts (rest, else whole): with a wrong
    eval -> frame offset d the true offset would belong to another frame and the error would be pixels."""
    regs = chain.get("R0_raw", {})
    reg = "rest" if "rest" in regs else "whole"
    err, g = (mean_of([regs.get(reg, {}).get(k, {}).get(key, NAN) for k in (2, 3)]) for key in ("median_err", "median_g"))
    return {"region": reg, "median_err_k2_3": err, "median_g_k2_3": g, "limit_px": D_CHECK_PX, "ok": bool(err < D_CHECK_PX),
            "valid": bool(g >= 2)} if err == err else {}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("view")
    ap.add_argument("--root", default=ROOT, required=ROOT is None)
    ap.add_argument("--mask", help="vegmask cell folder (default: <view>\\mask, else phase 1's)")
    a = ap.parse_args()
    view_dir = os.path.join(a.root, a.view)
    res = view_metrics(view_dir, a.mask)
    json.dump(res, open(os.path.join(view_dir, "metrics.json"), "w"), indent=1)
    print(f"{view_dir}: P={res.get('phase')} frames {res.get('frames')} runs {res['runs']}")
    for r, o in res["ofac"].items():
        print(f"  OFAC {r}: evals {o['evals']}, missing {o['missing']}" + (f" {o['missing_evals']}" if o["missing"] else ""))
    for n in res["notes"]:
        print("  note:", n)


if __name__ == "__main__":
    main()
