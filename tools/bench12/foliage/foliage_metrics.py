"""Metrics of the foliage frame-carry diagnosis (FOLIAGE_DIAG.md): M1-M5 per region for one view, and the
marker's reasons per region on the full passes (phase 2).

Usage: python foliage_metrics.py <view> [--root DIR]
Reads <root>\\<view>\\<case>\\r<k>\\ written by run_foliage.py; writes metrics.json and metrics.md into <root>\\<view>.
Missing colour cases are skipped (listed in "notes"); t1acc, gt8 and mask are required: they give the full-pass
frames and the regions.
--case R reads case R's r1/r2 as t1 (a marker rule, phase 2: t1r1..t1r4; t1nomm is then left out) and writes
metrics_R.json / metrics_R.md; t0, t1acc, gt8, t1vis and mask stay the view's own (the full passes do not move).
"""
import argparse, glob, json, os, re

import numpy as np
from PIL import Image

from f16io import REASONS, read_ofac, read_ofmv
from foliage_report import markdown
import bench_paths

ROOT = bench_paths.out("foliage")
N = 8                                            # TemporalEvery of the t1 cases
CLASSES = {1: "grass", 2: "leaves", 3: "fern", 4: "trunk"}
FOLIAGE = (1, 2, 3)
D_CANDIDATES = range(-N, N + 1)
D_MARGIN = 2.0                                   # the best d's exact-match share must exceed 2x the second best
PAIRS = {"t1_r1": ("t1_r1", "t0_r1"), "t1_r2": ("t1_r2", "t0_r2"), "t1nomm": ("t1nomm_r1", "t0_r1"),
         "t0_self": ("t0_r2", "t0_r1")}          # t0_self: the bench's own run-to-run difference
M2_REGIONS = ("foliage", "edge", "rest")
M35_REGIONS = ("foliage",) + tuple(CLASSES[k] for k in FOLIAGE)
REASON_REGIONS = ("foliage", "edge", "rest") + tuple(CLASSES.values())
NAN = float("nan")


# ---- primitives (test_foliage_metrics.py) ---------------------------------------------------------------------
def _shift(m, dy, dx):
    out = np.zeros_like(m)
    h, w = m.shape
    out[max(dy, 0):h + min(dy, 0), max(dx, 0):w + min(dx, 0)] = \
        m[max(-dy, 0):h + min(-dy, 0), max(-dx, 0):w + min(-dx, 0)]
    return out


def _morph(m, r, grow):
    """Square (2r+1) dilation (grow) or erosion of a bool mask by numpy shifts; outside the image counts as False."""
    out = m.copy()
    for dy in range(-r, r + 1):
        for dx in range(-r, r + 1):
            out = (out | _shift(m, dy, dx)) if grow else (out & _shift(m, dy, dx))
    return out


def regions(cls):
    """cls: uint8 class map (0 none/sky, 1 grass, 2 leaves, 3 fern, 4 trunk) -> bool masks by region and class."""
    fol = np.isin(cls, FOLIAGE)
    dil = _morph(fol, 2, True)
    r = {"foliage": fol, "edge": dil ^ _morph(fol, 2, False), "rest": ~dil}   # rest: no sky split (ruling)
    for k, name in CLASSES.items():
        r[name] = cls == k
    return r


def psnr(mse):
    return float("inf") if mse == 0 else float(10 * np.log10(255.0 ** 2 / mse))


def psnr_masked(a, b, m):
    return psnr(float(np.mean((a[m].astype(np.float64) - b[m]) ** 2))) if m.any() else NAN


def by_offset(values, first_full, n):
    """{frame: value} -> {offset since the last full pass: mean}; NaN values (empty masks) are left out."""
    groups = {}
    for f, v in values.items():
        if not np.isnan(v):
            groups.setdefault((f - first_full) % n, []).append(v)
    return {o: float(np.mean(v)) for o, v in sorted(groups.items())}


def flicker(error_maps, mask=None):
    """Mean |e_t - e_{t-1}| over the mask, averaged over the consecutive pairs."""
    m = np.ones(error_maps[0].shape, bool) if mask is None else mask
    return float(np.mean([np.mean(np.abs(b[m] - a[m])) for a, b in zip(error_maps, error_maps[1:])]))


def _lowpass(x, m, block):
    h, w = (x.shape[0] // block) * block, (x.shape[1] // block) * block
    tiles = lambda a: a[:h, :w].reshape(h // block, block, w // block, block).sum(axis=(1, 3))
    cover = tiles(m.astype(np.float64))
    return tiles(np.where(m, x, 0.0)) / np.maximum(cover, 1), cover >= block * block / 2


def flicker_low(error_maps, mask=None, block=16):
    """flicker() of the maps box-averaged over block x block tiles (lowflick.py), on tiles at least half masked."""
    m = np.ones(error_maps[0].shape, bool) if mask is None else mask
    low = [_lowpass(e, m, block) for e in error_maps]
    return flicker([x for x, _ in low], low[0][1]) if low[0][1].any() else NAN


def chain_error(acc, gt, mask, model=None):
    """Share of mask pixels with |acc - gt| > 1 px and > 4 px. With the model field given, pixels whose model
    motion is the "none" marker (x >= 4*width/2) are left out."""
    valid = mask if model is None else mask & (model[..., 0] < 2 * model.shape[1])
    n = int(np.count_nonzero(valid))
    if n == 0:
        return {"gt1px": NAN, "gt4px": NAN}
    err = np.hypot(*(acc[valid] - gt[valid]).T)
    return {"gt1px": np.count_nonzero(err > 1) / n, "gt4px": np.count_nonzero(err > 4) / n}


def reason_shares(reason, mask):
    """Share of the mask's pixels per marker reason (0 kept .. 4 non-finite, f16io.REASONS); NaN for an empty mask."""
    n = int(np.count_nonzero(mask))
    return [np.count_nonzero(reason[mask] == k) / n if n else NAN for k in REASONS]


# ---- data access ---------------------------------------------------------------------------------------------
def frames_of(d, pattern=r"dump_(\d+)\.png"):
    return sorted(int(m.group(1)) for f in os.listdir(d) if (m := re.fullmatch(pattern, f)))


def rgb(d, f):
    return np.asarray(Image.open(os.path.join(d, f"dump_{f}.png")).convert("RGB"), np.float32)


def luma(img):
    return img @ np.array([0.2126, 0.7152, 0.0722], np.float32)


def classes(mask_dir, f):
    img = np.asarray(Image.open(os.path.join(mask_dir, f"dump_{f}.png")).convert("RGB"))
    r = img[..., 0]
    if (r % 50).any() or img[..., 1:].any() or r.max() > 200:
        raise ValueError(f"{mask_dir} frame {f}: not a vegmask dump (R = class*50, G = B = 0)")
    return (r // 50).astype(np.uint8)


def gt_motion(gt_dir, f, shape, notes):
    """The span-8 field of frame f at the motion texture's shape (nearest resample if it differs, noted)."""
    _, gt = read_ofmv(os.path.join(gt_dir, f"motion_{f}.f32"))
    if gt.shape[:2] != shape:
        notes.add(f"gt8 motion {gt.shape[:2]} resampled (nearest) to the motion texture {shape}")
        ys, xs = np.arange(shape[0]) * gt.shape[0] // shape[0], np.arange(shape[1]) * gt.shape[1] // shape[1]
        gt = gt[ys][:, xs] * np.array([shape[1] / gt.shape[1], shape[0] / gt.shape[0]], np.float32)
    return gt


def length(v):
    return np.hypot(v[..., 0], v[..., 1])


# ---- full passes ---------------------------------------------------------------------------------------------
def derive_d(acc_paths, gt_dir, notes):
    """frame = eval + d. Each candidate d is scored by the share of moving non-marker pixels whose chained offset
    matches motion-span 8 exactly (< 0.05 px: a smooth chain is exact to half-float level, a frame off is not);
    the best d must be unique (above D_MARGIN x the second best)."""
    gt_frames = set(frames_of(gt_dir, r"motion_(\d+)\.f32"))
    scores = {}
    for path in acc_paths:
        head, model, chain, _ = read_ofac(path)
        live = model[..., 0] < 2 * head["width"]
        for d in D_CANDIDATES:
            if head["eval"] + d in gt_frames:
                gt = gt_motion(gt_dir, head["eval"] + d, model.shape[:2], notes)
                moving = live & ((length(gt) > 0.5) | (length(chain) > 0.5))
                scores.setdefault(d, []).append(float(np.mean(length(chain - gt)[moving] < 0.05)) if moving.any() else NAN)
    mean = {d: float(np.nanmean(v)) for d, v in scores.items()}
    order = sorted(mean, key=mean.get, reverse=True)
    assert len(order) >= 2 and mean[order[0]] > D_MARGIN * mean[order[1]], f"eval->frame offset d not unique: {mean}"
    return order[0], mean


def full_pass_frames(acc_dir, gt_dir, notes, d=None):
    paths = sorted(glob.glob(os.path.join(acc_dir, "ofps_acc_*.bin")))
    assert paths, f"no ofps_acc_*.bin in {acc_dir}"
    if d is None:
        d, scores = derive_d(paths, gt_dir, notes)
    else:   # a uniform turn makes the span-8 field the same on every frame: d is then set, and t1vis must confirm it
        scores = {}
        notes.add(f"d = {d} given, not derived; checked against t1vis' full-pass frames")
    full = {int(re.search(r"ofps_acc_(\d+)\.bin$", p).group(1)) + d: p for p in paths}
    assert all((f - min(full)) % N == 0 for f in full), f"full passes not on one offset: {sorted(full)}"
    return d, scores, full


# ---- metrics -------------------------------------------------------------------------------------------------
def m3(full, gt_dir, mask_dir, notes):
    """Chained offset and model motion against motion-span 8 on the full-pass frames, foliage and per class; and from
    OFAC v2 the marker's reasons -> (M3, {region: {reason: share of the region's pixels}}, {} for v1 dumps)."""
    rows = {r: [] for r in M35_REGIONS}
    why = {r: [] for r in REASON_REGIONS}
    gt_frames, mask_frames = set(frames_of(gt_dir, r"motion_(\d+)\.f32")), set(frames_of(mask_dir))
    for f, path in sorted(full.items()):
        if f not in gt_frames or f not in mask_frames:
            continue
        _, model, chain, reason = read_ofac(path)
        regs = regions(classes(mask_dir, f))
        assert regs["foliage"].shape == model.shape[:2], "vegmask and motion texture sizes differ"
        gt = gt_motion(gt_dir, f, model.shape[:2], notes)
        marker = model[..., 0] >= 2 * model.shape[1]
        for r in M35_REGIONS:
            c, m = chain_error(chain, gt, regs[r], model), chain_error(model, gt, regs[r], model)
            rows[r].append([c["gt1px"], c["gt4px"], m["gt1px"], m["gt4px"],
                            float(marker[regs[r]].mean()) if regs[r].any() else NAN])
        if reason is None:
            notes.add("OFAC v1 acc dumps: no marker reasons")
            continue
        assert np.array_equal(reason != 0, marker), f"{path}: the reason plane disagrees with the model's markers"
        for r in REASON_REGIONS:
            why[r].append(reason_shares(reason, regs[r]))
    keys = ("chain_gt1px", "chain_gt4px", "model_gt1px", "model_gt4px", "marker")
    res = {r: dict(frames=len(v), **dict(zip(keys, np.nanmean(v, axis=0).tolist() if v else [NAN] * 5)))
           for r, v in rows.items()}
    return res, {r: dict(zip(REASONS, np.nanmean(v, axis=0).tolist())) for r, v in why.items() if v}


def frame_loop(dirs, frames, offset):
    """M1 (per frame MSE and MAD by pair and region), M2 (flicker) and M5 (acceptance) in one pass."""
    m1 = {p: {} for p in PAIRS if all(dirs.get(x) for x in PAIRS[p])}
    m2 = {r: {k: {} for k in ("t1", "t1_low", "t0", "t0_low")} for r in M2_REGIONS}
    m5 = {r: np.zeros(6) for r in M35_REGIONS}   # pixels, w<0.5 pixels, |err| and err^2 sums for w<0.5 and w>=0.5
    colour = [k for k, v in dirs.items() if v and k not in ("mask_r1", "t1acc_r1", "gt8_r1")]
    prev, vis_full = None, []
    for f in frames:
        regs = regions(classes(dirs["mask_r1"], f))
        img = {k: rgb(dirs[k], f) for k in colour}
        for p in m1:
            diff = img[PAIRS[p][0]] - img[PAIRS[p][1]]
            for r, m in regs.items():
                dm = diff[m].astype(np.float64)
                m1[p].setdefault(r, {})[f] = (float(np.mean(dm ** 2)), float(np.mean(np.abs(dm)))) if dm.size else (NAN, NAN)
        if "t1_r1" in img and "t0_r1" in img:
            cur = (f, luma(img["t1_r1"]) - luma(img["t0_r1"]), luma(img["t0_r1"]))
            if prev is not None and prev[0] == f - 1:
                for r in M2_REGIONS:
                    m, e, t = regs[r], [prev[1], cur[1]], [prev[2], cur[2]]
                    m2[r]["t1"][f], m2[r]["t1_low"][f] = flicker(e, m), flicker_low(e, m)
                    m2[r]["t0"][f], m2[r]["t0_low"][f] = flicker(t, m), flicker_low(t, m)
            prev = cur
        if "t1vis_r1" in img and img["t1vis_r1"][..., 0].max() > 0:
            vis_full.append(f)                                    # the vis replaces carried frames only (R = 0)
        if offset(f) != 0 and "t1vis_r1" in img and "t1_r1" in img and "t0_r1" in img:
            low_w = img["t1vis_r1"][..., 2] / 255 < 0.5          # B = acceptance weight w on carried frames
            err = (img["t1_r1"] - img["t0_r1"]).astype(np.float64)
            for r in M35_REGIONS:
                lo, hi = regs[r] & low_w, regs[r] & ~low_w
                m5[r] += [regs[r].sum(), lo.sum(), np.abs(err[lo]).sum(), (err[lo] ** 2).sum(),
                          np.abs(err[hi]).sum(), (err[hi] ** 2).sum()]
    if "t1vis_r1" in colour:   # independent check of d: t1vis' full-pass frames are the ones not replaced by the vis
        expect = [f for f in frames if offset(f) == 0]
        assert vis_full == expect, f"t1vis full-pass frames {vis_full} != eval + d {expect}"
    return m1, m2, m5


def summarize_m1(m1, first_full):
    out = {}
    for p, per_region in m1.items():
        out[p] = {}
        for r, vals in per_region.items():
            mse = by_offset({f: v[0] for f, v in vals.items()}, first_full, N)
            mad = by_offset({f: v[1] for f, v in vals.items()}, first_full, N)
            carried = [o for o in mad if o != 0]
            # carry's share of the loss on carried frames: 1 - offset 0 / mean of offsets 1..7
            mean = lambda x: float(np.mean([x[o] for o in carried])) if carried else 0.0
            share = lambda x: 1 - x[0] / mean(x) if 0 in x and mean(x) > 0 else NAN
            out[p][r] = {"psnr": {o: psnr(v) for o, v in mse.items()}, "mad": mad,
                         "carry_share_mad": share(mad), "carry_share_mse": share(mse)}
    return out


def repeat_check(s1):
    if "t1_r1" not in s1 or "t1_r2" not in s1:
        return {}
    out = {}
    for r, cell in s1["t1_r1"].items():
        a, b = cell["psnr"], s1["t1_r2"].get(r, {}).get("psnr", {})
        out[r] = {}
        for o in a:
            if o in b:
                delta = 0.0 if a[o] == b[o] else abs(a[o] - b[o])
                out[r][o] = {"delta_db": delta, "unstable": bool(delta > 0.1)}
    return out


def decision(s1, repeat):
    """The A/B hint from the foliage carry share (t1 r1, MAD), or "unverified: <reason>" when the t1 repeats are
    missing or any foliage cell (foliage, edge, grass, leaves, fern; any offset) differs by more than 0.1 dB."""
    if not repeat:
        return {"hint": "unverified: t1 r2 or t0 r2 missing", "verified": False}
    bad = [f"{r}@{o}" for r in ("foliage", "edge", "grass", "leaves", "fern") for o, c in repeat.get(r, {}).items()
           if c["unstable"]]
    if bad:
        return {"hint": f"unverified: repeats differ > 0.1 dB at {', '.join(bad)}", "verified": False}
    share = s1.get("t1_r1", {}).get("foliage", {}).get("carry_share_mad", NAN)
    return {"hint": "n/a" if np.isnan(share) else "B (carry)" if share >= 0.5 else "A (full frame)", "verified": True}


def summarize_m4(s1):
    t1, nomm = s1.get("t1_r1", {}), s1.get("t1nomm", {})
    return {r: {"d_psnr_offset0": nomm[r]["psnr"][0] - t1[r]["psnr"][0], "d_mad_offset0": nomm[r]["mad"][0] - t1[r]["mad"][0]}
            for r in t1 if r in nomm and 0 in t1[r]["mad"] and 0 in nomm[r]["mad"]}


def summarize_m2(m2, first_full):
    out = {}
    for r, per in m2.items():
        out[r] = {k: float(np.nanmean(list(v.values()))) if v else NAN for k, v in per.items()}
        out[r]["t1_by_offset"] = by_offset(per["t1"], first_full, N)
    return out


def summarize_m5(m5):
    out = {}
    for r, (n, nlo, alo, slo, ahi, shi) in m5.items():
        nhi = n - nlo
        out[r] = {"pixels": int(n), "w_below_half": nlo / n if n else NAN,
                  "mad_w_low": alo / (3 * nlo) if nlo else NAN, "psnr_w_low": psnr(slo / (3 * nlo)) if nlo else NAN,
                  "mad_w_high": ahi / (3 * nhi) if nhi else NAN, "psnr_w_high": psnr(shi / (3 * nhi)) if nhi else NAN}
    return out


def view_metrics(view_dir, given_d=None, t1_case="t1"):
    notes = set()
    dirs = {}
    for case, rep in [("t0", 1), ("t0", 2), ("t1", 1), ("t1", 2), ("t1vis", 1), ("t1nomm", 1),
                      ("t1acc", 1), ("gt8", 1), ("mask", 1)]:
        d = os.path.join(view_dir, t1_case if case == "t1" else case, f"r{rep}")
        dirs[f"{case}_r{rep}"] = d if os.path.isdir(d) and not (case == "t1nomm" and t1_case != "t1") else None
    for k in ("t1acc_r1", "gt8_r1", "mask_r1"):
        assert dirs[k], f"{k} missing under {view_dir}"
    notes.update(f"{k} missing: skipped" for k, v in dirs.items() if not v)
    assert given_d is None or dirs["t1vis_r1"], "--d needs t1vis to confirm the full-pass frames"
    d, d_scores, full = full_pass_frames(dirs["t1acc_r1"], dirs["gt8_r1"], notes, given_d)
    first_full = min(full)
    frames = frames_of(dirs["mask_r1"])
    for k, v in dirs.items():
        if v and k not in ("mask_r1", "gt8_r1", "t1acc_r1"):
            frames = [f for f in frames if os.path.exists(os.path.join(v, f"dump_{f}.png"))]
    m1, m2, m5 = frame_loop(dirs, frames, lambda f: (f - first_full) % N)
    m3res, reasons = m3(full, dirs["gt8_r1"], dirs["mask_r1"], notes)
    s1 = summarize_m1(m1, first_full)
    repeat = repeat_check(s1)
    verdict = decision(s1, repeat)
    return {"view": os.path.basename(view_dir), "frames": [frames[0], frames[-1], len(frames)] if frames else [],
            "n": N, "d": d, "d_scores": d_scores, "full_pass_frames": sorted(full), "first_full": first_full,
            "M1": s1, "repeat": repeat, "verified": verdict["verified"], "M2": summarize_m2(m2, first_full),
            "M3": m3res, "m3_reasons": reasons, "M4": summarize_m4(s1),
            "M5": summarize_m5(m5), "decision_hint": verdict["hint"], "notes": sorted(notes)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("view")
    ap.add_argument("--root", default=ROOT, required=ROOT is None)
    ap.add_argument("--d", type=int, help="eval->frame offset when the data cannot single it out (checked by t1vis)")
    ap.add_argument("--case", default="t1", help="the case read as t1, e.g. t1r1 (a marker rule)")
    a = ap.parse_args()
    view_dir = os.path.join(a.root, a.view)
    res = dict(view_metrics(view_dir, a.d, a.case), t1_case=a.case)
    name = "metrics" if a.case == "t1" else f"metrics_{a.case}"
    json.dump(res, open(os.path.join(view_dir, f"{name}.json"), "w"), indent=1)
    open(os.path.join(view_dir, f"{name}.md"), "w", encoding="utf-8").write(markdown(res))
    print(f"{view_dir}: d={res['d']} full passes {res['full_pass_frames']} decision hint {res['decision_hint']}")


if __name__ == "__main__":
    main()
