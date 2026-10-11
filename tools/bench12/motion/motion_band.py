"""Raw fields of a motion view (motion/README.md): the true offset g, the off-screen mask and the disocclusion band.

g and the full pass's depth come from R1_raw (OFMV: the offset to the last full pass; OFDP), this frame's depth from the
`prevdepth` run (OFPD: the depth each pixel's surface had in the camera of the full pass). Without that run the band
falls back to phase 2's approximation (this frame's own depth, not moved to the full pass's camera), which grows with
camera motion; metrics.json then says band_rule "approximate".
One mask per cycle: the off-band region of frames P + 8c .. P + 8c + 7 is the complement of the union of their bands,
so every offset of a cycle is measured on the same pixels.
"""
import os

import numpy as np

from f16io import read_ofdp, read_ofmv, read_ofpd
from ghost_metrics import FAR, band, view_depth

N = 8
TRUTH, PREV = "R1_raw", "prevdepth"


def offscreen_mask(g):
    """True where the source p + g (ghost_metrics' floor(p + 0.5 + g)) falls outside the frame."""
    h, w = g.shape[:2]
    ys, xs = np.mgrid[0:h, 0:w]
    qx, qy = np.floor(xs + 0.5 + g[..., 0]), np.floor(ys + 0.5 + g[..., 1])
    return (qx < 0) | (qx >= w) | (qy < 0) | (qy >= h)


def disocclusion_band(now, then, g, rel=0.05):
    """View depths in metres: `now` of p in the full pass's camera, `then` the full pass's depth map; g the true
    offset -> bool band (ghost_metrics.band: the source off screen or a relative depth difference above rel)."""
    return band(now, then, g, rel)


def raw_fields(truth, prev, f, k):
    """-> (g, off-screen mask, band, honest) of frame f at span k, or None without R1_raw's files."""
    if not truth:
        return None
    paths = [os.path.join(truth, p) for p in (f"motion_{f}.f32", f"depth_{f}.f32", f"depth_{f - k}.f32")]
    if not all(os.path.exists(p) for p in paths):
        return None
    g = read_ofmv(paths[0])[1]
    then = view_depth(read_ofdp(paths[2])[1])
    pd = os.path.join(prev, f"prevdepth_{f}.f32") if prev else ""
    honest = os.path.exists(pd)
    if honest:
        z = read_ofpd(pd)[1].astype(np.float64)
        now = np.where(z > 0, z, FAR)                               # the sky has no surface: as far as the far plane
    else:
        now = view_depth(read_ofdp(paths[1])[1])
    return g, offscreen_mask(g), disocclusion_band(now, then, g), honest


def frame_fields(runs, phase, frames, notes):
    """Pass over R1_raw (and prevdepth): per frame the packed off-screen mask, per cycle the union of the bands, the
    band share by offset (own band and the cycle's union). None without R1_raw."""
    truth, prev = runs.get(TRUTH), runs.get(PREV)
    if not truth:
        return None
    off, union, own, rules = {}, {}, {}, set()
    for f in frames:
        o = (f - phase) % N
        raw = raw_fields(truth, prev, f, o or N)
        if raw is None:
            continue
        _, out, b, honest = raw
        rules.add("prevdepth" if honest else "approximate")
        off[f] = (np.packbits(out), out.shape)
        c = (f - phase) // N
        union[c] = b if c not in union else union[c] | b
        own.setdefault(o, []).append(float(b.mean()))
    if not off:
        notes.add(f"{TRUTH} has no raw files for the measured frames: no band, no off-screen split")
        return None
    if rules != {"prevdepth"}:
        notes.add("approximate disocclusion band on some frames (no prevdepth run or file): off-band numbers are estimates")
    share_u = {}
    for f in off:
        share_u.setdefault((f - phase) % N, []).append(float(union[(f - phase) // N].mean()))
    return {"off": off, "union": union, "rule": "prevdepth" if rules == {"prevdepth"} else "approximate",
            "band_share": {o: float(np.mean(v)) for o, v in sorted(own.items())},
            "union_share": {o: float(np.mean(v)) for o, v in sorted(share_u.items())}}


def unpack(entry):
    packed, shape = entry
    return np.unpackbits(packed, count=shape[0] * shape[1]).reshape(shape).astype(bool)
