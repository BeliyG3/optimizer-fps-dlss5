"""Verify existing plan-2 captures; never launch a runtime or modify references."""
import argparse
import json
import sys
from pathlib import Path

from compare_ngx_audit import compare

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "bench"))
from compare_reference import compare_pair, report


def manifest(directory):
    return json.loads((directory / "manifest.json").read_text(encoding="utf-8-sig"))


def images(reference, candidate, runs, names=None):
    results = []
    for run in runs:
        name = run["name"]
        target = names[name] if names else name
        limit = None if str(run["temporal"]) == "0" else 0.05
        for frame in (120, 239):
            a = reference / f"{name}_dump_{frame}.bmp"
            b = candidate / f"{target}_dump_{frame}.bmp"
            result = compare_pair(a, b)
            code = report(str(b), result, limit)
            results.append(dict(case=target, frame=frame, limit=limit, exit=code, **result))
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--candidate", default="candidate_p2t7")
    parser.add_argument("--output-root", type=Path,
                        help="Root containing run_addon/run_r521/run_nrhost captures")
    args = parser.parse_args()
    root = Path(__file__).resolve().parent
    candidate_root = args.output_root or root
    nr = candidate_root / "run_nrhost" / args.candidate
    mapping = {"warp_t0": "core_warp", "off_t0": "core_off", "warp_t1": "core_warp_t1"}
    audits = {}
    for shell, core in mapping.items():
        audits[core] = compare(nr / f"{shell}_ngx_audit.log",
                               nr / "core" / f"{core}_ngx_audit.log", off=shell == "off_t0")
    (nr / "ngx_audit_comparison.json").write_text(json.dumps(audits, indent=2) + "\n")
    failed = False
    for runtime in ("run_addon", "run_r521", "run_nrhost"):
        directory = root / runtime
        reference = directory / "reference_before_2026.9.1"
        candidate = candidate_root / runtime / args.candidate
        ref_manifest = manifest(reference)
        captured = manifest(candidate)
        expected = {r["name"] for r in ref_manifest["runs"]}
        actual = {r["name"] for r in captured["runs"]}
        if expected != actual or not all(r["exit"] == 0 and r["ok"] for r in captured["runs"]):
            raise ValueError(f"Missing/failed runtime cases: {runtime}")
        results = images(reference, candidate, ref_manifest["runs"])
        if runtime == "run_nrhost":
            core_manifest = manifest(candidate / "core")
            if {r["name"] for r in core_manifest["runs"]} != set(mapping.values()):
                raise ValueError("Missing core cases")
            if not all(r["exit"] == 0 and r["ok"] for r in core_manifest["runs"]):
                raise ValueError("Failed core case")
            core_runs = [r for r in ref_manifest["runs"] if r["name"] in mapping]
            results += images(reference, candidate / "core", core_runs, mapping)
        output = dict(source_sha=captured["source_sha"], candidate=args.candidate,
                      reference=str(reference), comparisons=results)
        (candidate / "frame_comparison.json").write_text(json.dumps(output, indent=2) + "\n")
        failed |= any(r["exit"] for r in results)
        print(f"{runtime}: {len(results)} pairs, maximum MAD {max(r['mad'] for r in results)}")
    return int(failed)


if __name__ == "__main__":
    sys.exit(main())
