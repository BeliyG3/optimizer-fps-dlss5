"""Ghost guard of foliage carry phase 2 (FOLIAGE_DIAG.md): the error against T0 inside the disocclusion band, where the
model would blend its old picture of an occluder into the surface it uncovered (the 26.28 ghost).

Usage: python ghost_metrics.py <guard view> [--root DIR] [--d N]
Guard views: lab_combo, garden_walkers (= the garden `combo` folder, window 952-1079), the static-camera lab_ghost,
lab_ghost_chad, lab_ghost_door and their _raster twins (dumps 48-175). Reads t0 r1/r2, t1 r1/r2, t1nomm
(the guard's "t1off": DebugTemporalNoModelMotion=1), t1acc, gt8 and gtdepth from <root>\\<folder>\\; writes
metrics.json["ghost"] there (other keys kept).

Band at frame t: p with ground truth g = gt8(p) (span 8, render px) and source q = floor(p + 0.5 + g) is in the band
if q is off screen or |z_t(p) - z_{t-8}(q)| / max(z_t(p), z_{t-8}(q)) > 0.05, z the view depth in metres from gtdepth.
Approximation: z_t(p) is used as is, not propagated to the expected depth at t-8 as the core does; a surface whose
distance changes more than 5 % in 8 frames (a fast approach) is counted as a disocclusion.
"""
import argparse, json, os

import numpy as np

from f16io import read_ofdp, read_ofmv
from foliage_metrics import N, NAN, PAIRS, ROOT, by_offset, frames_of, full_pass_frames, psnr, rgb

GUARD = {"lab_combo": "lab_combo", "garden_walkers": "combo",   # guard view -> folder under the root
         "lab_ghost": "lab_ghost", "lab_ghost_raster": "lab_ghost_raster",
         "lab_ghost_chad": "lab_ghost_chad", "lab_ghost_chad_raster": "lab_ghost_chad_raster",
         "lab_ghost_door": "lab_ghost_door", "lab_ghost_door_raster": "lab_ghost_door_raster"}
REL = 0.05
NEAR, FAR = 0.1, 300.0                                          # math.h Perspective, raster_depth.hlsli
SENSITIVE_DB = 0.5                                              # t1off must lose this much in the band (offset 0 or 1)


def view_depth(z):
    """Standard hardware depth z/w (the bench's default --depth) -> view depth in metres; z = 1 (sky) -> FAR."""
    return NEAR * FAR / (FAR - z.astype(np.float64) * (FAR - NEAR))


def band(cur, prev, gt, rel=REL):
    """cur, prev: view depth [h, w] at t and t-8; gt: span-8 motion [h, w, 2] at t -> bool band [h, w]."""
    h, w = cur.shape
    ys, xs = np.mgrid[0:h, 0:w]
    qx, qy = np.floor(xs + 0.5 + gt[..., 0]).astype(np.int64), np.floor(ys + 0.5 + gt[..., 1]).astype(np.int64)
    off = (qx < 0) | (qx >= w) | (qy < 0) | (qy >= h)
    src = prev[np.clip(qy, 0, h - 1), np.clip(qx, 0, w - 1)]
    return off | (np.abs(cur - src) / np.maximum(np.maximum(cur, src), 1e-6) > rel)


def resize_nearest(m, shape):
    if m.shape == shape:
        return m
    return m[np.arange(shape[0]) * m.shape[0] // shape[0]][:, np.arange(shape[1]) * m.shape[1] // shape[1]]


def frame_band(depth_dir, gt_dir, f):
    _, gt = read_ofmv(os.path.join(gt_dir, f"motion_{f}.f32"))
    cur = view_depth(read_ofdp(os.path.join(depth_dir, f"depth_{f}.f32"))[1])
    prev = view_depth(read_ofdp(os.path.join(depth_dir, f"depth_{f - N}.f32"))[1])
    assert cur.shape == gt.shape[:2], f"frame {f}: depth {cur.shape} and gt8 {gt.shape[:2]} sizes differ"
    return band(cur, prev, gt)


def ghost_metrics(view_dir, given_d=None, t1_case="t1"):
    notes = set()
    dirs = {f"{c}_r{r}": os.path.join(view_dir, t1_case if c == "t1" else c, f"r{r}") for c, r in
            [("t0", 1), ("t0", 2), ("t1", 1), ("t1", 2), ("t1nomm", 1), ("t1acc", 1), ("gt8", 1), ("gtdepth", 1)]}
    for k in ("t0_r1", "t1_r1", "t1nomm_r1", "t1acc_r1", "gt8_r1", "gtdepth_r1"):
        assert os.path.isdir(dirs[k]), f"{k} missing under {view_dir}"
    d, _, full = full_pass_frames(dirs["t1acc_r1"], dirs["gt8_r1"], notes, given_d)
    first_full = min(full)
    pairs = {p: v for p, v in PAIRS.items() if all(os.path.isdir(dirs[x]) for x in v)}
    notes.update(f"pair {p} skipped: a case is missing" for p in PAIRS if p not in pairs)
    gt_frames = set(frames_of(dirs["gt8_r1"], r"motion_(\d+)\.f32"))
    depth_frames = set(frames_of(dirs["gtdepth_r1"], r"depth_(\d+)\.f32"))
    colour = sorted({x for v in pairs.values() for x in v})
    frames = [f for f in frames_of(dirs["t0_r1"]) if f in gt_frames and f in depth_frames and f - N in depth_frames
              and all(os.path.exists(os.path.join(dirs[k], f"dump_{f}.png")) for k in colour)]
    assert frames, f"no frame has t0, gt8 and gtdepth at t and t-{N}"
    share, mse, mad = {}, {p: {} for p in pairs}, {p: {} for p in pairs}
    for f in frames:
        img = {k: rgb(dirs[k], f) for k in colour}
        b = frame_band(dirs["gtdepth_r1"], dirs["gt8_r1"], f)
        if b.shape != img["t0_r1"].shape[:2]:
            notes.add(f"band {b.shape} resized (nearest) to the colour dumps {img['t0_r1'].shape[:2]}")
            b = resize_nearest(b, img["t0_r1"].shape[:2])
        share[f] = float(b.mean())
        for p, (a, ref) in pairs.items():
            diff = (img[a][b] - img[ref][b]).astype(np.float64)
            mse[p][f], mad[p][f] = (float(np.mean(diff ** 2)), float(np.mean(np.abs(diff)))) if diff.size else (NAN, NAN)
    res = {"view": os.path.basename(view_dir), "frames": [frames[0], frames[-1], len(frames)], "d": d,
           "first_full": first_full, "rel": REL, "band_share": by_offset(share, first_full, N),
           "band_share_mean": float(np.mean(list(share.values()))), "pairs": {}}
    for p in pairs:
        m = by_offset(mse[p], first_full, N)
        res["pairs"]["t1off" if p == "t1nomm" else p] = {"psnr": {o: psnr(v) for o, v in m.items()}, "mad": by_offset(mad[p], first_full, N)}
    if "t1_r1" in res["pairs"] and "t1off" in res["pairs"]:
        t1, off = res["pairs"]["t1_r1"]["psnr"], res["pairs"]["t1off"]["psnr"]
        loss = {o: t1[o] - off[o] for o in (0, 1, 2) if o in t1 and o in off}
        res["t1off_loss_db"] = loss
        res["sensitive"] = any(loss.get(o, NAN) >= SENSITIVE_DB for o in (0, 1))
    res["notes"] = sorted(notes)
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("view", choices=sorted(GUARD))
    ap.add_argument("--root", default=ROOT, required=ROOT is None)
    ap.add_argument("--d", type=int, help="eval->frame offset when the data cannot single it out (combo: 0, phase 1)")
    ap.add_argument("--case", default="t1", help="the case read as t1 (a marker rule, t1r1..t1r4) -> metrics.json"
                    "[\"ghost_<case>\"]; t1off stays the markers-off run, so t1off_loss_db is then the rule's")
    a = ap.parse_args()
    view_dir = os.path.join(a.root, GUARD[a.view])
    res = dict(ghost_metrics(view_dir, a.d, a.case), guard_view=a.view, t1_case=a.case)
    path = os.path.join(view_dir, "metrics.json")
    metrics = json.load(open(path)) if os.path.exists(path) else {}
    metrics["ghost" if a.case == "t1" else f"ghost_{a.case}"] = res
    json.dump(metrics, open(path, "w"), indent=1)
    print(json.dumps({k: res[k] for k in ("guard_view", "frames", "band_share_mean", "t1off_loss_db", "sensitive")
                      if k in res}))
    for p, v in res["pairs"].items():
        print(p, " ".join(f"{o}: {v['psnr'][o]:.2f} dB / {v['mad'][o]:.3f}" for o in sorted(v["psnr"])))


if __name__ == "__main__":
    main()
