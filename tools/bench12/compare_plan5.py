"""Compare named plan-5 compute captures with their pixel counterparts."""

import argparse
import hashlib
import json
import sys
from pathlib import Path

import numpy as np
from PIL import Image

from compare_ngx_audit import compare as compare_audit

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "bench"))
from compare_reference import compare_pair


FRAMES = (120, 239)
RUNTIMES = ("run_addon", "run_r521", "run_nrhost")
CORE_NAMES = {"warp_t0": "core_warp", "off_t0": "core_off", "warp_t1": "core_warp_t1"}
REQUIRED_BINARIES = (
    "optimizer-fps-dlss5-core.dll",
    "optimizer-fps-dlss5/warp_pack_cs.dxbc",
    "optimizer-fps-dlss5/warp_unpack_cs.dxbc",
)


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest().upper()


def read_manifest(directory, expected_path):
    path = directory / "manifest.json"
    data = json.loads(path.read_text(encoding="utf-8-sig"))
    runs = data.get("runs")
    if not runs:
        raise ValueError(f"Empty runs: {path}")
    names = [run["name"] for run in runs]
    if len(set(names)) != len(names):
        raise ValueError(f"Duplicate cases: {path}")
    for run in runs:
        if run["exit"] != 0 or not run["ok"]:
            raise ValueError(f"Failed capture: {path} {run['name']}")
        if run.get("warp_path") != expected_path:
            raise ValueError(f"Wrong DebugWarpPath: {path} {run['name']}")
        effective = "none" if str(run["mode"]) == "0" else expected_path
        if run.get("effective_warp_path") != effective:
            raise ValueError(f"Effective warp path missing or wrong: {path} {run['name']}")
    binaries = {name.replace("\\", "/"): value
                for name, value in data.get("binaries", {}).items()}
    for name in REQUIRED_BINARIES:
        if not binaries.get(name):
            raise ValueError(f"Missing binary hash: {path} {name}")
    data["binaries"] = binaries
    return data, sha256(path)


def exact_region_mask(first, second, destination):
    with Image.open(first) as a, Image.open(second) as b:
        left = np.asarray(a.convert("RGB"), dtype=np.uint8)
        right = np.asarray(b.convert("RGB"), dtype=np.uint8)
    if left.shape != right.shape:
        raise ValueError(f"Image extent differs: {first} {second}")
    mask = np.any(left != right, axis=2)
    ys, xs = np.nonzero(mask)
    if len(xs):
        Image.fromarray(np.uint8(mask) * 255, "L").save(destination)
        bounds = [int(xs.min()), int(ys.min()), int(xs.max()) + 1, int(ys.max()) + 1]
    else:
        bounds = None
    return int(mask.sum()), bounds


def validate_audits(pixel, compute, core=False):
    results = {}
    for path, directory in (("pixel", pixel), ("compute", compute)):
        names = CORE_NAMES.values() if core else CORE_NAMES.keys()
        results[path] = {}
        for name in names:
            audit = directory / f"{name}_ngx_audit.log"
            # Parsing rejects absent, empty and truncated audit blocks.
            results[path][name] = compare_audit(audit, audit, off="off" in name)
    return results


def compare_directories(pixel, compute, limit, core=False):
    a, a_hash = read_manifest(pixel, "pixel")
    b, b_hash = read_manifest(compute, "compute")
    left = {run["name"]: run for run in a["runs"]}
    right = {run["name"]: run for run in b["runs"]}
    if left.keys() != right.keys():
        raise ValueError(f"Run inventory differs: {pixel} {compute}")
    audits = validate_audits(pixel, compute, core) if core or pixel.parent.name == "run_nrhost" else {}
    rows = []
    failed = False
    for name, run in left.items():
        for frame in FRAMES:
            first = pixel / f"{name}_dump_{frame}.bmp"
            second = compute / f"{name}_dump_{frame}.bmp"
            if not first.is_file() or not second.is_file():
                raise FileNotFoundError(f"Missing pair: {first} {second}")
            result = compare_pair(first, second)
            if result["status"] == "size":
                raise ValueError(f"Image extent differs: {first} {second}")
            mask_path = compute / f"{name}_diff_mask_{frame}.png"
            changed, bounds = exact_region_mask(first, second, mask_path)
            if not changed and mask_path.exists():
                mask_path.unlink()
            off = str(run["mode"]) == "0"
            passed = result["status"] == "identical" if off else result["mad"] <= limit
            rows.append({"case": name, "frame": frame, "off": off,
                         "passed": passed, "changed_pixels": changed,
                         "diff_bounds_xyxy": bounds,
                         "diff_mask": mask_path.name if changed else None, **result})
            failed |= not passed
    output = {
        "pixel": str(pixel), "compute": str(compute),
        "mad_limit_rgb8": limit,
        "pixel_manifest_sha256": a_hash, "compute_manifest_sha256": b_hash,
        "pixel_binaries": {name: a["binaries"][name] for name in REQUIRED_BINARIES},
        "compute_binaries": {name: b["binaries"][name] for name in REQUIRED_BINARIES},
        "audits": audits, "comparisons": rows,
    }
    target = compute / "plan5_comparison.json"
    target.write_text(json.dumps(output, indent=2) + "\n", encoding="utf-8")
    print(f"{compute}: {len(rows)} pairs, maximum MAD "
          f"{max(row['mad'] for row in rows):.6f}, pass={not failed}")
    return failed


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pixel", required=True)
    parser.add_argument("--compute", required=True)
    parser.add_argument("--output-root", type=Path,
                        help="Root containing run_addon/run_r521/run_nrhost captures")
    parser.add_argument("--mad-limit-rgb8", type=float, default=0.35)
    args = parser.parse_args()
    if not 0 <= args.mad_limit_rgb8 <= 0.35:
        parser.error("MAD limit must be between 0 and 0.35 RGB8 values")
    root = args.output_root or Path(__file__).resolve().parent
    failed = False
    for runtime in RUNTIMES:
        directory = root / runtime
        failed |= compare_directories(directory / args.pixel,
                                      directory / args.compute,
                                      args.mad_limit_rgb8)
    failed |= compare_directories(root / "run_nrhost" / args.pixel / "core",
                                  root / "run_nrhost" / args.compute / "core",
                                  args.mad_limit_rgb8, core=True)
    return int(failed)


if __name__ == "__main__":
    sys.exit(main())
