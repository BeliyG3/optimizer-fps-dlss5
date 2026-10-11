"""The OFAC dumps of a motion run: the cadence phase P and the dump window's continuity (motion/README.md).
Imported by run_motion.py (after each run) and motion_metrics.py; needs no numpy."""
import os, re, struct

N = 8


def phase_from_evals(evals, n=N):
    """Full-pass evals (frame = eval, d = 0 as in phase 1) -> the cadence phase P; ValueError unless all are = P mod n."""
    phases = {e % n for e in evals}
    if len(phases) != 1:
        raise ValueError(f"full-pass evals {sorted(evals)[:12]} are not on one phase mod {n}")
    return phases.pop()


def ofac_heads(d):
    """{eval: fullPassIndex} from the 32-byte headers of d's ofps_acc_<eval>.bin."""
    out = {}
    for name in os.listdir(d):
        if m := re.fullmatch(r"ofps_acc_(\d+)\.bin", name):
            with open(os.path.join(d, name), "rb") as f:
                head = f.read(32)
            ev, full = struct.unpack_from("<II", head, 16)
            if head[:4] != b"OFAC" or ev != int(m.group(1)):
                raise ValueError(f"{d}\\{name}: not an OFAC dump of eval {m.group(1)}")
            out[ev] = full
    return out


def check_run(cell, r0_cell, n=N, window=None, offset=0, skip=()):
    """P from R0's full-pass dumps. Without a window the run's dumps must be exactly R0's. With the dump window
    [lo, hi) every eval is dumped: the full passes are the evals = P (mod n), and the core's full-pass index must
    step after exactly those, across lost evals too; evals missing from the window are reported (the single readback buffer drops frames
    when the CPU runs ahead). offset: a run from --start-frame S counts its evals from S (frame = eval + S), so its evals
    are moved by S and compared with R0's from its first one on. skip: R0's evals the run cannot dump (a --nr-reset-at
    frame: the core's chain starts there, nothing to read back, as on a short run's first evaluate). ValueError on any mismatch."""
    ref = sorted(ofac_heads(r0_cell))
    phase = phase_from_evals(ref, n)
    heads = {e + offset: full for e, full in ofac_heads(cell).items()}
    if not heads:
        raise ValueError(f"{cell}: no OFAC dumps")
    evs = sorted(heads)
    if offset:
        ref = [e for e in ref if e >= evs[0]]
    ref = [e for e in ref if e not in skip]
    if window is None:
        if phase_from_evals(evs, n) != phase or evs != ref:
            raise ValueError(f"{cell}: full-pass evals {evs[:8]}.. differ from R0's {ref[:8]}..")
        missing, full = [e for e in range(evs[0], evs[-1] + 1, n) if e not in heads], evs
    else:
        lo, hi = window
        stray = [e for e in evs if not lo <= e < hi]
        # the index counts the full passes before an eval, so between two dumped evals a < b (a lost one between them
        # included) it must grow by the number of evals = P in [a, b)
        bad = [a for a, b in zip(evs, evs[1:]) if heads[b] - heads[a] != sum(e % n == phase for e in range(a, b))]
        if stray or bad:
            raise ValueError(f"{cell}: evals outside the window {stray[:8]}, index steps off the cadence P={phase} at {bad[:8]}")
        missing, full = [e for e in range(lo, hi) if e not in heads], [e for e in evs if e % n == phase]
    return {"phase": phase, "evals": [evs[0], evs[-1], len(evs)], "missing": len(missing), "missing_evals": missing[:32],
            "full": full}
