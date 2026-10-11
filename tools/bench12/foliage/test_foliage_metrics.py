import os, struct, tempfile
import numpy as np
from foliage_metrics import regions, psnr_masked, by_offset, flicker, chain_error
def test_regions():
    cls = np.zeros((10, 10), np.uint8); cls[3:7, 3:7] = 1
    r = regions(cls)
    assert r["foliage"].sum() == 16 and (r["edge"] & ~r["foliage"]).any() and not (r["rest"] & r["foliage"]).any()
def test_psnr_masked():
    a = np.zeros((4, 4, 3)); b = a.copy(); b[0, 0] = 255
    m = np.zeros((4, 4), bool); m[0, 0] = True
    assert abs(psnr_masked(a, b, m) - 0.0) < 1e-6 and psnr_masked(a, b, ~m) == float("inf")
def test_by_offset():
    vals = {f: float(f % 8) for f in range(48, 176)}
    assert by_offset(vals, first_full=48, n=8) == {o: float(o) for o in range(8)}
def test_flicker_constant_error_is_zero():
    e = [np.full((4, 4), 3.0)] * 3
    assert flicker(e) == 0.0
def test_chain_error_counts_px():
    gt = np.zeros((2, 2, 2), np.float32); acc = gt.copy(); acc[0, 0] = (2, 0); acc[1, 1] = (5, 0)
    m = np.ones((2, 2), bool)
    assert chain_error(acc, gt, m) == {"gt1px": 0.5, "gt4px": 0.25}
def test_decision_needs_stable_repeats():
    from foliage_metrics import decision, repeat_check
    cell = lambda p1: {"foliage": {"psnr": {0: 30.0, 1: p1}, "carry_share_mad": 0.2}}
    stable = {"t1_r1": cell(30.0), "t1_r2": cell(30.05)}
    assert decision(stable, repeat_check(stable)) == {"hint": "A (full frame)", "verified": True}
    shaky = {"t1_r1": cell(30.0), "t1_r2": cell(30.2)}
    v = decision(shaky, repeat_check(shaky))
    assert v["hint"].startswith("unverified") and "foliage@1" in v["hint"] and not v["verified"]
    alone = {"t1_r1": cell(30.0)}
    v = decision(alone, repeat_check(alone))
    assert v["hint"].startswith("unverified") and "missing" in v["hint"] and not v["verified"]
def _ofac(path, version, model, chain, reason=None):
    h, w = model.shape[:2]
    with open(path, "wb") as f:
        f.write(b"OFAC" + struct.pack("<IIIIIff", version, w, h, 17, 2, 1.0, 1.0))
        f.write(model.astype(np.float32).tobytes() + chain.astype(np.float32).tobytes())
        if reason is not None:
            f.write(reason.astype(np.uint8).tobytes())
def test_ofac_v2_reason_mix():
    from f16io import read_ofac
    from foliage_metrics import reason_shares
    reason = np.array([[0, 1, 2, 2], [3, 3, 3, 4]], np.uint8)        # known mix: 1 kept, 1 off, 2 depth, 3 colour, 1 NaN
    chain = np.full((2, 4, 2), 0.5, np.float32)
    model = np.where((reason != 0)[..., None], np.array([16.0, 0.0], np.float32), chain)
    with tempfile.TemporaryDirectory(dir=os.path.dirname(os.path.abspath(__file__))) as d:
        _ofac(os.path.join(d, "v2.bin"), 2, model, chain, reason)
        _ofac(os.path.join(d, "v1.bin"), 1, model, chain)
        head, m, c, r = read_ofac(os.path.join(d, "v2.bin"))
        assert head["version"] == 2 and head["eval"] == 17 and (r == reason).all() and (m == model).all() and (c == chain).all()
        assert read_ofac(os.path.join(d, "v1.bin"))[3] is None
        _ofac(os.path.join(d, "short.bin"), 2, model, chain)       # v2 without its reason plane
        try:
            read_ofac(os.path.join(d, "short.bin"))
            assert False, "a v2 file without the reason plane was accepted"
        except ValueError:
            pass
    assert reason_shares(r, np.ones_like(r, bool)) == [1 / 8, 1 / 8, 2 / 8, 3 / 8, 1 / 8]
    top = np.zeros_like(r, bool); top[0] = True
    assert reason_shares(r, top) == [0.25, 0.25, 0.5, 0.0, 0.0]
def _square(x, w, h=4, size=(12, 24)):
    """View depth: a flat background at 10 m, a w x h block at 5 m whose left edge is at column x (rows 4..4+h)."""
    d = np.full(size, 10.0, np.float32)
    d[4:4 + h, x:x + w] = 5.0
    return d
def test_ghost_band_uncovered_strip():
    from ghost_metrics import band
    for w in (4, 8):          # a 4x4 square moved 8 px uncovers its 4x4 footprint; an 8x4 block moved 8 px its 8x4 strip
        prev, cur = _square(2, w), _square(10, w)
        gt = np.zeros(cur.shape + (2,), np.float32)
        gt[4:8, 10:10 + w, 0] = -8                    # the block's pixels come from 8 px to the left; the background stays
        expect = np.zeros(cur.shape, bool)
        expect[4:8, 2:2 + w] = True                   # the old footprint (not covered again: the move is >= w)
        b = band(cur, prev, gt)
        assert b.sum() == w * 4 and (b == expect).all(), (w, np.argwhere(b))
    flat = np.full((4, 6), 10.0, np.float32)
    pan = np.zeros((4, 6, 2), np.float32); pan[..., 0] = -2   # the view moved: the 2 left columns come from off screen
    b = band(flat, flat, pan)
    assert b[:, :2].all() and not b[:, 2:].any()
def test_ofdp_round_trip():
    from f16io import read_ofdp
    d = np.arange(6, dtype=np.float32).reshape(2, 3) / 7
    with tempfile.TemporaryDirectory(dir=os.path.dirname(os.path.abspath(__file__))) as t:
        path = os.path.join(t, "depth_40.f32")
        open(path, "wb").write(b"OFDP" + struct.pack("<III", 3, 2, 40) + d.tobytes())
        frame, got = read_ofdp(path)
        assert frame == 40 and (got == d).all()
        open(path, "wb").write(b"OFDP" + struct.pack("<III", 3, 2, 40) + d.tobytes()[:-4])
        try:
            read_ofdp(path)
            assert False, "a short OFDP file was accepted"
        except ValueError:
            pass
if __name__ == "__main__":
    for name, fn in list(globals().items()):
        if name.startswith("test_"): fn()
    print("ok")
