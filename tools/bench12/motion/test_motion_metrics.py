"""Synthetic checks of motion_metrics' primitives. pytest-compatible; without pytest: python test_motion_metrics.py."""
import os, struct, sys, tempfile

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from PIL import Image
from motion_metrics import (chain_metrics, chain_stats, check_run, colour_metrics, disocclusion_band, frame_fields, full_cycles,
                            offscreen_mask, offset_of, phase_from_evals, raw_fields, warmup_end)
from ghost_metrics import view_depth
from motion_report import IMAGE_VIEWS, evaluate, pick

HERE = os.path.dirname(os.path.abspath(__file__))


def raises(fn, *args):
    try:
        fn(*args)
    except ValueError:
        return True
    return False


def test_phase_from_evals():
    assert phase_from_evals([49, 57, 65], 8) == 1
    assert raises(phase_from_evals, [49, 58], 8)
    assert raises(phase_from_evals, [], 8)


def test_offset_of():
    assert offset_of(frame=50, phase=1, n=8) == 1
    assert offset_of(57, 1, 8) == 0
    assert offset_of(56, 1, 8) == 7


def test_warmup_end():
    assert warmup_end(phase=1, n=8) == 9                     # full passes 1, 9: frames < 9 are excluded
    assert warmup_end(phase=0, n=8) == 8


def test_full_cycles():
    assert full_cycles(list(range(7, 27)), phase=1, n=8) == list(range(9, 25))   # 7-8 and 25-26: partial cycles out
    assert full_cycles([9, 10], 1, 8) == []


def test_offscreen_mask():
    g = np.zeros((4, 4, 2), np.float32)
    g[..., 0] = 3                                            # source p + (3, 0)
    off = offscreen_mask(g)
    assert not off[:, 0].any() and off[:, 1:].all()          # x = 0 -> 3 inside [0, 4); x >= 1 -> 4.. outside
    assert not offscreen_mask(np.zeros((4, 4, 2), np.float32)).any()


def test_chain_stats():
    g = np.zeros((4, 4, 2), np.float32)
    g[..., 0] = 8
    mv = np.zeros_like(g)
    mv[..., 0] = 1                                           # one frame's vector
    s = chain_stats(g / 4, g, mv)
    assert abs(s["coverage"] - 0.25) < 1e-6 and abs(s["median_err"] - 6) < 1e-6 and s["median_g"] == 8
    s = chain_stats(g.copy(), g, mv)
    assert s["coverage"] == 1 and s["median_err"] == 0 and s["p90_err"] == 0 and s["restart_share"] == 0
    a = g.copy()
    a[0, 0] = mv[0, 0]                                       # the chain restarted from this frame's vector
    assert abs(chain_stats(a, g, mv)["restart_share"] - 1 / 16) < 1e-9
    assert np.isnan(chain_stats(a, g, g)["restart_share"])   # |g - mv| <= 1 everywhere: nothing detectable


def test_disocclusion_band():
    now = np.full((3, 3), 0.5, np.float32)
    then = now.copy()
    then[1, 2] = 0.7
    g = np.zeros((3, 3, 2), np.float32)
    b = disocclusion_band(now, then, g)
    assert b[1, 2] and b.sum() == 1


def _ofac(d, ev, full, chain=None, carry=None):
    """v2 (model, chain, reason); with `carry` v3, the carry chain as a fourth plane."""
    h, w = chain.shape[:2] if chain is not None else (1, 1)
    plane = (chain if chain is not None else np.zeros((1, 1, 2))).astype(np.float32).tobytes()
    tail = b"" if carry is None else carry.astype(np.float32).tobytes()
    with open(os.path.join(d, f"ofps_acc_{ev}.bin"), "wb") as f:
        f.write(b"OFAC" + struct.pack("<IIIIIff", 2 if carry is None else 3, w, h, ev, full, 1.0, 1.0) + plane + plane +
                bytes(w * h) + tail)


def _raw(d, magic, name, f, a):
    with open(os.path.join(d, f"{name}_{f}.f32"), "wb") as out:
        out.write(magic + struct.pack("<III", a.shape[1], a.shape[0], f) + a.astype(np.float32).tobytes())


def test_check_run():
    with tempfile.TemporaryDirectory(dir=os.path.dirname(os.path.abspath(__file__))) as t:
        r0, r1, win = (os.path.join(t, x) for x in ("R0", "R1", "R1_raw"))
        for d in (r0, r1, win):
            os.makedirs(d)
        for i, ev in enumerate((49, 57, 65)):
            _ofac(r0, ev, i + 1)
            _ofac(r1, ev, i + 1)
        assert check_run(r0, r0, 8)["phase"] == 1 and check_run(r1, r0, 8)["missing"] == 0
        _ofac(r1, 70, 4)                                     # an eval off the cadence
        assert raises(check_run, r1, r0, 8)
        idx = {e: 2 + (e > 57) + (e > 65) for e in range(54, 70)}   # full passes 57, 65: the index steps after them
        for e in (e for e in idx if e != 60):                # eval 60 dropped (single readback buffer)
            _ofac(win, e, idx[e])
        res = check_run(win, r0, 8, window=(54, 70))
        assert res["missing"] == 1 and res["missing_evals"] == [60] and res["full"] == [57, 65]
        _ofac(win, 60, 9)                                    # an index step on a carried frame: wrong cadence
        assert raises(check_run, win, r0, 8, (54, 70))
        os.remove(os.path.join(win, "ofps_acc_60.bin"))
        os.remove(os.path.join(win, "ofps_acc_65.bin"))      # a lost full pass: 64 -> 66 must step by one
        assert check_run(win, r0, 8, (54, 70))["missing_evals"] == [60, 65]
        _ofac(win, 66, idx[66] + 1)                          # ... and a wrong step across the gap is caught
        assert raises(check_run, win, r0, 8, (54, 70))


def test_short_run():
    from run_motion import short_start
    assert short_start(952, 32, 1) == 920 and short_start(952, 64, 1) == 888 and short_start(952, 32, 3) == 914
    with tempfile.TemporaryDirectory(dir=os.path.dirname(os.path.abspath(__file__))) as t:
        r0, s, bad = (os.path.join(t, x) for x in ("R0", "R0_s32", "R0_bad"))
        for d in (r0, s, bad):
            os.makedirs(d)
        for i, ev in enumerate((41, 49, 57, 65)):
            _ofac(r0, ev, i + 1)
        for i, ev in enumerate((9, 17, 25)):                 # from S = 40: eval e is frame e + 40, R0's 49, 57, 65
            _ofac(s, ev, i + 1)
            _ofac(bad, ev + 1, i + 1)                        # one frame off the phase
        assert check_run(s, r0, 8, None, 40)["full"] == [49, 57, 65] and raises(check_run, bad, r0, 8, None, 40)
        z = os.path.join(t, "R0_z")                                        # --nr-reset: the reset eval has no dump
        os.makedirs(z)
        for i, ev in enumerate((41, 57, 65)):
            _ofac(z, ev, i + 1)
        assert check_run(z, r0, 8, None, 0, [49])["full"] == [41, 57, 65] and raises(check_run, z, r0, 8, None, 0)


def test_variants():
    from run_variants import setting_ids, variant_keys, split_outputs
    ids = setting_ids()
    assert ids["Mode"] == 0 and ids["TemporalMode"] == 17 and ids["TemporalEvery"] == 18
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "no_such_out")
    assert raises(variant_keys, "combo", ["T0", "C1"], 1080, 952, out)     # C1 needs R0 (and its key is a diagnostic)
    with tempfile.TemporaryDirectory(dir=os.path.dirname(os.path.abspath(__file__))) as t:
        rt, cells = os.path.join(t, "rt"), [os.path.join(t, c) for c in ("a", "b")]
        os.makedirs(rt)
        for name in ("v0_dump_952.png", "v1_dump_952.png", "ofps_acc_9.bin", "ofps_acc_168.bin"):
            open(os.path.join(rt, name), "wb").close()
        assert split_outputs(rt, cells, 160) == [0, 159]                    # each variant evaluates frames S+1 .. S+159
        assert sorted(os.listdir(cells[0])) == ["dump_952.png", "ofps_acc_9.bin"]
        assert sorted(os.listdir(cells[1])) == ["dump_952.png", "ofps_acc_168.bin"]


def test_no_truth_run():
    assert raw_fields(None, None, 10, 1) is None
    notes = set()
    assert frame_fields({"T0": "x"}, 1, [9, 10], notes) is None


def test_colour_and_band_end_to_end():
    """Phase 1: frames 9 (full) .. 16 form cycle 1. T0 is flat; R0 errs by 10 at one pixel on frame 10 (offset 1),
    exactly where the prevdepth run puts a disocclusion: the band holds that pixel, the off-band region does not."""
    with tempfile.TemporaryDirectory(dir=HERE) as t:
        runs = {k: os.path.join(t, k) for k in ("T0", "R0", "R1_raw", "prevdepth")}
        for d in runs.values():
            os.makedirs(d)
        z = np.full((4, 8), 0.99, np.float32)
        metres = view_depth(z).astype(np.float32)
        for f in range(1, 17):
            _raw(runs["R1_raw"], b"OFDP", "depth", f, z)
            _raw(runs["R1_raw"], b"OFMV", "motion", f, np.zeros((4, 8, 2)))
            pd = metres.copy()
            if f == 10:
                pd[1, 2] *= 2
            _raw(runs["prevdepth"], b"OFPD", "prevdepth", f, pd)
        for f in range(9, 17):
            img = np.full((4, 8, 3), 100, np.uint8)
            Image.fromarray(img).save(os.path.join(runs["T0"], f"dump_{f}.png"))
            if f == 10:
                img[1, 2] += 10
            Image.fromarray(img).save(os.path.join(runs["R0"], f"dump_{f}.png"))
        notes = set()
        fields = frame_fields(runs, 1, list(range(9, 17)), notes)
        assert fields["rule"] == "prevdepth" and abs(fields["band_share"][1] - 1 / 32) < 1e-9 and fields["band_share"][2] == 0
        assert abs(fields["union_share"][0] - 1 / 32) < 1e-9          # the cycle's union also covers its full pass
        m1, m2, off = colour_metrics(runs, 1, list(range(9, 17)), None, fields, notes)
        assert abs(m1["R0"]["whole"][1]["psnr"] - 10 * np.log10(255 ** 2 / (100 / 32))) < 1e-6
        assert m1["R0"]["whole"][2]["psnr"] == float("inf") and m1["R0"]["whole_offband"][1]["psnr"] == float("inf")
        assert off["R0"]["share_out"] == 0 and m2["R0"]["whole"]["flicker"] > 0


def test_chain_metrics_end_to_end():
    """Offset 2 of phase 1 (eval 11): the true offset (8, 0), this frame's vector (1, 0). R0's chain covers a quarter;
    R1's oracle chain is the truth, and its own vector is the truth too, so no restart is detectable there (NaN)."""
    with tempfile.TemporaryDirectory(dir=HERE) as t:
        runs = {k: os.path.join(t, k) for k in ("R0_raw", "R1_raw")}
        for d in runs.values():
            os.makedirs(d)
        g, mv = np.zeros((4, 8, 2)), np.zeros((4, 8, 2))
        g[..., 0], mv[..., 0] = 8, 1
        _raw(runs["R1_raw"], b"OFMV", "motion", 11, g)
        _raw(runs["R0_raw"], b"OFMV", "motion", 11, mv)
        _ofac(runs["R0_raw"], 11, 2, g / 4)
        _ofac(runs["R1_raw"], 11, 2, g)
        out = chain_metrics(runs, 1, 9, None, set())
        r0, r1 = out["R0_raw"]["whole"][2], out["R1_raw"]["whole"][2]
        assert abs(r0["coverage"] - 0.25) < 1e-6 and abs(r0["median_err"] - 6) < 1e-6 and r0["restart_share"] == 0
        assert r1["coverage"] == 1 and r1["median_err"] == 0 and np.isnan(r1["restart_share"])
        # step 1c: S5_raw's v3 dump holds the model's chain (R0's, a quarter) and the carry chain (the truth); the
        # statistics read the carry plane, a v2 dump of S2_raw the model's
        for r, carry in (("S5_raw", g), ("S2_raw", None)):
            runs[r] = os.path.join(t, r)
            os.makedirs(runs[r])
            _raw(runs[r], b"OFMV", "motion", 11, mv)
            _ofac(runs[r], 11, 2, g / 4, carry)
        planes = {}
        out = chain_metrics(runs, 1, 9, None, set(), planes)
        assert planes == {"R0_raw": "model", "R1_raw": "model", "S5_raw": "carry", "S2_raw": "model"}, planes
        assert out["S5_raw"]["whole"][2]["coverage"] == 1 and out["S2_raw"]["whole"][2] == out["R0_raw"]["whole"][2]


def test_model_frames():
    """Criterion 6 of step 1c: the model's frames (offsets 0 and 1) identical to R0 on all six views."""
    from motion_report import model_frames
    views = {v: {"offset_identity": {"S5": {"0": [16, 16], "1": [16, 16], "2": [0, 16]}}} for v in IMAGE_VIEWS}
    assert model_frames(views, "S5")["ok"]
    views["pan"]["offset_identity"]["S5"]["1"] = [15, 16]
    assert not model_frames(views, "S5")["ok"] and "pan" in model_frames(views, "S5")["text"]
    del views["pan"]
    assert not model_frames(views, "S5")["ok"]                        # a missing view fails


def _view(psnr7=30.0, flick=10.0):
    whole = lambda p7: {str(o): {"psnr": p7 if o == 7 else 30.0} for o in range(8)}
    m1 = {"R0": {"whole": whole(30.0), "band": whole(30.0)}, "C1": {"whole": whole(psnr7), "band": whole(30.0)}}
    m2 = {"R0": {g: {"flicker": 10.0} for g in ("foliage", "whole")},
          "C1": {"foliage": {"flicker": flick}, "whole": {"flicker": 10.0}}}
    return {"n": 8, "m1": m1, "m2": m2}


def test_criteria_evaluator():
    """C1 is 5 % less flickery on foliage everywhere, cheap, neutral in mode 3 and in the ghost band, but offset 7 of held
    drops 0.2 dB (the mean of 1-7 only 0.03): it fails exactly criterion 2, and no winner is picked."""
    views = {v: _view(30.0, 9.5) for v in IMAGE_VIEWS}
    views["held"] = _view(29.8, 9.5)
    views["combo"]["mode3"] = {"base": {"R0": {"whole": 27.79}, "C1": {"whole": 27.78}}}
    e = evaluate(views, "C1", {"C1": 0.1})
    assert [k for k in range(1, 6) if not e[k]["ok"]] == [2], {k: e[k]["text"] for k in e}
    assert abs(e[2]["worst"][2][0] + 0.2) < 1e-9 and e[2]["worst"][2][1] == "held"
    assert abs(e[1]["gains"]["combo"] - 0.05) < 1e-9 and pick(views, {"C1": e}) is None
    views["held"] = _view(30.0, 9.5)
    assert pick(views, {"C1": evaluate(views, "C1", {"C1": 0.1})}) == "C1"
    assert not evaluate(views, "C1", {})[5]["ok"]                     # cost not measured: fails
    del views["ghost_door"]
    e = evaluate(views, "C1", {"C1": 0.1})
    assert not e[2]["ok"] and not e[3]["ok"]                          # a missing view fails, it does not pass by default


def test_split_regions():
    """motion_split: a level floor z = 1000 / (y - 40) below the horizon, a near box (1.5 m) on it, a far wall above;
    the box outline is an edge, the floor is found, the box is near, and the regions are exclusive."""
    from motion_split import regions_of
    h, w = 400, 600                                           # the border (48 px) stays clear of the box
    y = np.arange(h)[:, None].repeat(w, 1).astype(np.float64)
    z = np.where(y > 60, 1000.0 / np.maximum(y - 40, 1), 50.0)
    z[250:300, 200:320] = 1.5
    r = regions_of(z, None, (h, w))
    assert sum(m.astype(int) for m in r.values()).max() == 1 and not r["band"].any()
    assert r["floor"][200:240, 120:180].all() and not r["floor"][48:55, 60:540].any()
    assert r["near"][260:290, 210:310].all() and r["edge"][250, 200:320].all() and r["border"][:48].all()


if __name__ == "__main__":
    for name, fn in list(globals().items()):
        if name.startswith("test_"):
            fn()
    print("ok")
