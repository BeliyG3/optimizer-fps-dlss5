"""Markdown tables of one view's foliage metrics (the dict foliage_metrics.view_metrics returns)."""
from f16io import REASONS

NAN = float("nan")


def _f(x, digits=2):
    return "-" if x is None or x != x else f"{x:.{digits}f}"


def _table(head, rows):
    return "\n".join(["| " + " | ".join(head) + " |", "|" + "---|" * len(head)] +
                     ["| " + " | ".join(str(c) for c in r) + " |" for r in rows]) + "\n"


def _m1(res, pair):
    cells = res["M1"].get(pair, {})
    rep = res["repeat"] if pair in ("t1_r1", "t1_r2") else {}
    offsets = range(res["n"])
    u = "" if res.get("verified") else " unverified"
    rows = []
    for r, c in cells.items():
        mark = lambda o: " unstable" if rep.get(r, {}).get(o, {}).get("unstable") else ""
        rows.append([r, "PSNR dB"] + [_f(c["psnr"].get(o)) + mark(o) for o in offsets] + [_f(c["carry_share_mse"]) + u])
        rows.append(["", "MAD"] + [_f(c["mad"].get(o)) + mark(o) for o in offsets] + [_f(c["carry_share_mad"]) + u])
    return f"#### {pair}\n\n" + _table(["region", ""] + [str(o) for o in offsets] + ["carry share"], rows)


def _reasons(why):
    if not why:
        return "\n## Marker reasons\n\nNo reason plane (OFAC v1 acc dumps).\n"
    names = [REASONS[k] for k in sorted(REASONS) if k]
    rows = []
    for r, shares in why.items():
        s = {int(k): v for k, v in shares.items()}
        marked = 1 - s[0]
        rows.append([r, _f(marked, 3)] + [f"{_f(s[k], 3)} ({_f(100 * s[k] / marked if marked else NAN, 0)} %)"
                                           for k in sorted(s) if k])
    return ("\n## Marker reasons on full-pass frames (OFAC v2)\n\nShare of the region's pixels the model gets the "
            "\"none\" marker for, and by the first test that failed (in brackets: of the marked pixels).\n\n" +
            _table(["region", "marked"] + names, rows))


def markdown(res):
    out = [f"# Foliage metrics: {res['view']}\n",
           f"- Frames {res['frames']}, N = {res['n']}; eval -> frame offset d = {res['d']} "
           f"(scores {', '.join(f'{d}: {_f(s, 3)}' for d, s in sorted(res['d_scores'].items(), key=lambda x: int(x[0])))}).",
           f"- Full-pass frames: {res['full_pass_frames']} (offset 0 = these frames).",
           f"- Decision hint (foliage carry share by MAD, t1 r1): **{res['decision_hint']}**."]
    out += [f"- Note: {n}" for n in res["notes"]]
    out.append("\n## M1: error against T0 by offset since the last full pass\n")
    out.append("Carry share = 1 - offset 0 / mean of offsets 1..7 (PSNR row: by MSE, MAD row: by MAD). "
               "`unstable`: the two t1 repeats differ by more than 0.1 dB in that cell. `unverified`: a foliage cell "
               "is unstable or a repeat is missing, so the carry shares, M4 and the hint do not count.\n")
    out += [_m1(res, p) for p in res["M1"]]
    out.append("## M2: flicker (mean |e_t - e_t-1|, luma 0..255; low = 16 px tiles)\n")
    out.append(_table(["region", "T1 error", "T1 error low", "T0 change", "T0 change low"],
                      [[r, _f(v["t1"], 3), _f(v["t1_low"], 3), _f(v["t0"], 3), _f(v["t0_low"], 3)]
                       for r, v in res["M2"].items()]))
    out.append("\n## M3: chained offset vs motion-span 8 on full-pass frames (marker pixels left out)\n")
    out.append(_table(["region", "frames", "chain >1 px", "chain >4 px", "model >1 px", "model >4 px", "marker share"],
                      [[r, v["frames"]] + [_f(v[k], 3) for k in
                                           ("chain_gt1px", "chain_gt4px", "model_gt1px", "model_gt4px", "marker")]
                       for r, v in res["M3"].items()]))
    out.append(_reasons(res.get("m3_reasons", {})))
    u = "" if res.get("verified") else " unverified"
    out.append("\n## M4: offset 0 with DebugTemporalNoModelMotion minus t1 r1\n")
    out.append(_table(["region", "d PSNR dB", "d MAD"],
                      [[r, _f(v["d_psnr_offset0"]) + u, _f(v["d_mad_offset0"], 3) + u] for r, v in res["M4"].items()]))
    out.append("\n## M5: acceptance weight w on carried frames (t1vis B channel) and the t1 r1 error\n")
    out.append(_table(["region", "pixels", "w < 0.5 share", "MAD w<0.5", "MAD w>=0.5", "PSNR w<0.5", "PSNR w>=0.5"],
                      [[r, v["pixels"], _f(v["w_below_half"], 3), _f(v["mad_w_low"]), _f(v["mad_w_high"]),
                        _f(v["psnr_w_low"]), _f(v["psnr_w_high"])] for r, v in res["M5"].items()]))
    return "\n".join(out) + "\n"
