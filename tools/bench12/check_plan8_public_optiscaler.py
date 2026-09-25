"""Plan 8 task 1: the add-on on top of a public, unpatched OptiScaler.

The public OptiScaler runs as dxgi.dll with its own NR ([DlssNr] Enabled) and loads ReShade
([Plugins] LoadReshade); ReShade loads the Optimizer FPS add-on, core and shaders.
Two stands: bench12 (native D3D12, DLSS RR through NGX, like 007 First Light) and pw_bench
(D3D11 through OptiScaler's D3D11-on-D3D12 bridge, like Fallen Order). Each case is one bench
run; the report compares frames with the OptiScaler-only control and between warp paths.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
BENCH = Path(__file__).resolve().parent
# Local stand template: bench12 with ReShade 6.8 (add-on build), NGX snippets and ReShade.ini.
TEMPLATE = BENCH / "run_fork_core/plan4_dual_p4t8"


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest().upper()


def set_ini(text: str, section: str, key: str, value: str) -> str:
    block = re.search(rf"(?ms)^\[{re.escape(section)}\]\r?\n.*?(?=^\[|\Z)", text)
    if not block:
        raise ValueError(f"Missing INI section {section}")
    changed, count = re.subn(rf"(?m)^{re.escape(key)}\s*=.*$", f"{key} = {value}", block[0])
    if count == 0:
        changed = changed.replace(f"[{section}]", f"[{section}]\n{key} = {value}", 1)
    elif count != 1:
        raise ValueError(f"Expected at most one {section}/{key}, found {count}")
    return text[:block.start()] + changed + text[block.end():]


def debug_errors(directory: Path) -> list[str]:
    errors = set()
    for name in ("sentinel_stdout.txt", "sentinel_stderr.txt"):
        for line in (directory / name).read_text(errors="replace").splitlines():
            if line.startswith("[d3d12:1]"):
                errors.add(re.sub(r"0x[0-9a-f]+", "0x<address>", line, flags=re.I))
    return sorted(errors)


FRAMES = (120, 239)
D3D11_BENCH = ROOT / "tools/bench/run"
D3D12_ARGS = ("240", "--host", "ngx", "--upscaler", "rr", "--sun-dir", "0.45,-0.77,0.45",
              "--sun-strength", "1500", "--exposure", "0.22", "--haze", "0.008", "--fov", "62",
              "--bloom", "0", "--dump", "120,239")
D3D11_ARGS = ("240", "--dlaa", "--fps-cap", "40", "--dump", "120,239")
HOOKED = "feature 18 create/evaluate/release hooked"
WARPED = "first warped evaluate completed"
CREATED = re.compile(r"feature 18 (created|adopted)[^\r\n]*", re.I)
TEMPORAL = re.compile(r"temporal mode (\d) running", re.I)
BACKGROUND = re.compile(r"background mode needs[^\r\n]*", re.I)
BAD = ("device was removed", "device_removed", "exception", "crash guard", "state journal unavailable")

# name: (stand, add-on loaded, Mode, TemporalMode, DebugWarpPath: 1 compute / 2 pixel)
CASES = {
    "control": ("d3d12", False, 0, 0, 1),
    "off": ("d3d12", True, 0, 0, 1),
    "pixel_t0": ("d3d12", True, 2, 0, 2),
    "pixel_t0_repeat": ("d3d12", True, 2, 0, 2),
    "compute_t0": ("d3d12", True, 2, 0, 1),
    "pixel_t1": ("d3d12", True, 2, 1, 2),
    "compute_t1": ("d3d12", True, 2, 1, 1),
    "compute_t3": ("d3d12", True, 2, 3, 1),
    "d3d11_control": ("d3d11", False, 0, 0, 1),
    "d3d11_off": ("d3d11", True, 0, 0, 1),
    "d3d11_pixel_t0": ("d3d11", True, 2, 0, 2),
    "d3d11_compute_t0": ("d3d11", True, 2, 0, 1),
    "d3d11_compute_t1": ("d3d11", True, 2, 1, 1),
    # wilsjo2 builds: NR before the upscaler (their main mode) and their own peripheral compression.
    "presr_control": ("d3d12", False, 0, 0, 1),
    "presr_pixel_t0": ("d3d12", True, 2, 0, 2),
    "own_spatial_control": ("d3d12", False, 0, 0, 1),
    "own_spatial_pixel_t0": ("d3d12", True, 2, 0, 2),
}
# name: (bench upscaler, [(section, key, value)] for OptiScaler.ini)
EXTRA = {
    "presr_control": ("sr", [("DlssNr", "RunBeforeSR", "true")]),
    "presr_pixel_t0": ("sr", [("DlssNr", "RunBeforeSR", "true")]),
    "own_spatial_control": ("rr", [("DlssNr", "SpatialCompression", "true")]),
    "own_spatial_pixel_t0": ("rr", [("DlssNr", "SpatialCompression", "true")]),
}


def stage(directory: Path, optiscaler: Path, case: tuple, extra: tuple) -> None:
    stand, addon, mode, temporal, warp = case
    if stand == "d3d12":
        shutil.copytree(TEMPLATE, directory, ignore=shutil.ignore_patterns(
            "*.log", "dump_*.bmp", "*stdout.txt", "*stderr.txt", "*.before-*.dll",
            "OptiScaler.ini", "optimizer-fps-dlss5*", "dxgi.dll", "nvngx.dll_dlssnr.dll",
            "nvngx.dll_optimizerfps.dll", "settings.json"))
        shutil.copy2(BENCH / "pw_bench12.exe", directory / "pw_bench12.exe")
        shutil.copy2(BENCH / "nvngx.dll_pwbench12.dll", directory / "nvngx.dll_pwbench12.dll")
    else:
        directory.mkdir(parents=True)
        for name in ("pw_bench.exe", "nvngx_dlss.dll", "nvngx_dlssnr.dll"):
            shutil.copy2(D3D11_BENCH / name, directory / name)
        for name in ("ReShade64.dll", "ReShade.ini"):
            shutil.copy2(TEMPLATE / name, directory / name)
    if optiscaler.is_dir():
        # A release folder as users unpack it into the game: OptiScaler.dll, its ini, its runtime folder.
        shutil.copy2(optiscaler / "OptiScaler.dll", directory / "dxgi.dll")
        shutil.copytree(optiscaler / "OptiScaler", directory / "OptiScaler", dirs_exist_ok=True)
        ini = (optiscaler / "OptiScaler.ini").read_text(encoding="utf-8-sig")
    else:
        # A clean build of the upstream tag: bin/x64/Release/a/OptiScaler.dll next to its forwarder.
        shutil.copy2(optiscaler, directory / "dxgi.dll")
        shutil.copy2(optiscaler.parent / "nvngx.dll_dlssnr.dll", directory / "nvngx.dll_dlssnr.dll")
        ini = (optiscaler.parents[3] / "OptiScaler.ini").read_text(encoding="utf-8-sig")
    ini = set_ini(ini, "Plugins", "LoadReshade", "true" if addon else "false")
    ini = set_ini(ini, "DlssNr", "Enabled", "true")
    ini = set_ini(ini, "Log", "LogToFile", "true")
    if stand == "d3d11":
        ini = set_ini(ini, "Upscalers", "Dx11Upscaler", "dlss_12")
    for section, key, value in extra[1]:
        ini = set_ini(ini, section, key, value)
    (directory / "OptiScaler.ini").write_text(ini, encoding="utf-8")
    if not addon:
        return
    build = ROOT / "out/build/x64"
    shutil.copy2(build / "hosts/reshade/Release/optimizer-fps-dlss5.addon64", directory)
    shutil.copy2(build / "core/Release/optimizer-fps-dlss5-core.dll", directory)
    shutil.copy2(build / "hosts/reshade/ngx_forwarder/Release/nvngx.dll_optimizerfps.dll", directory)
    (directory / "optimizer-fps-dlss5").mkdir()
    for shader in (build / "shaders").glob("*.dxbc"):
        shutil.copy2(shader, directory / "optimizer-fps-dlss5" / shader.name)
    reshade = (directory / "ReShade.ini").read_text(encoding="utf-8-sig")
    for key, value in (("Mode", mode), ("TemporalMode", temporal), ("DebugWarpPath", warp),
                       ("CrashGuard", 0)):
        reshade = set_ini(reshade, "OptimizerFPS", key, str(value))
    (directory / "ReShade.ini").write_text(reshade, encoding="utf-8")


def run(directory: Path, arguments: list[str], prefix: str) -> int:
    with (directory / f"{prefix}stdout.txt").open("wb") as out, \
         (directory / f"{prefix}stderr.txt").open("wb") as err:
        return subprocess.run(arguments, cwd=directory, stdout=out, stderr=err,
                              timeout=600).returncode


def capture(directory: Path, optiscaler: Path, case: tuple, extra: tuple) -> dict:
    stand, addon, mode, temporal, warp = case
    stage(directory, optiscaler, case, extra)
    upscaler = extra[0]
    scene = ROOT / "tools/bench/assets/lab_scene.glb"
    if stand == "d3d12":
        exe = str(directory / "pw_bench12.exe")
        bench_args = [*D3D12_ARGS[:3], "--gltf", str(scene), *D3D12_ARGS[3:]]
        bench_args[bench_args.index("--upscaler") + 1] = upscaler
        result = run(directory, [exe, *bench_args], "")
        sentinel = run(directory, [exe, "12", "--host", "ngx", "--gltf", str(scene), "--upscaler",
                                   upscaler, "--bloom", "0", "--core-state-sentinel", "compute",
                                   "--core-state-rebind", "--debug-layer"], "sentinel_")
        sentinel_log = (directory / "sentinel_stdout.txt").read_text(errors="replace")
        sentinel_state = "PASS" if sentinel == 0 and "core state sentinel: PASS" in sentinel_log else "FAIL"
        sentinel_errors = debug_errors(directory)
    else:
        result = run(directory, [str(directory / "pw_bench.exe"), *D3D11_ARGS], "")
        sentinel_state, sentinel_errors = "NOT_RUN", []  # the D3D11 stand has no state sentinel
    reshade_log = (directory / "ReShade.log").read_text(errors="replace") if addon else ""
    opti_log = (directory / "OptiScaler.log").read_text(errors="replace") \
        if (directory / "OptiScaler.log").is_file() else ""
    stdout = (directory / "stdout.txt").read_text(errors="replace")
    created, temporal_hit, background = (pattern.search(reshade_log)
                                         for pattern in (CREATED, TEMPORAL, BACKGROUND))
    lowered = (reshade_log + opti_log + stdout).lower()
    manifest = {
        "case": {"stand": stand, "addon": addon, "Mode": mode, "TemporalMode": temporal,
                 "DebugWarpPath": warp},
        "exit": result, "bench_finished": "device ok" in stdout,
        "hooked": HOOKED in reshade_log, "created": created[0] if created else "",
        "warped": WARPED in reshade_log, "compute_ready": "compute path ready" in reshade_log,
        "temporal_running": int(temporal_hit[1]) if temporal_hit else 0,
        "background_fallback_reason": background[0] if background else "",
        "bad_markers": [m for m in BAD if m in lowered],
        "sentinel": sentinel_state, "sentinel_debug_errors": sentinel_errors,
        "binaries": {name: sha256(directory / name) for name in
                     ("dxgi.dll", "optimizer-fps-dlss5.addon64", "optimizer-fps-dlss5-core.dll")
                     if (directory / name).is_file()},
    }
    (directory / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    return manifest


def mad(a: Path, b: Path, frame: int) -> float:
    with Image.open(a / f"dump_{frame}.bmp") as x, Image.open(b / f"dump_{frame}.bmp") as y:
        return float(np.abs(np.asarray(x.convert("RGB"), dtype=np.int16) -
                            np.asarray(y.convert("RGB"), dtype=np.int16)).mean())


def verdict(root: Path, results: dict) -> list[str]:
    problems = []
    for name, m in results.items():
        stand, addon, mode, temporal, warp = CASES[name]
        if m["exit"] != 0 or not m["bench_finished"] or m["bad_markers"]:
            problems.append(f"{name}: exit {m['exit']}, finished {m['bench_finished']}, {m['bad_markers']}")
        if m["sentinel"] not in ("PASS", "NOT_RUN") or (stand == "d3d12" and m["sentinel"] != "PASS"):
            problems.append(f"{name}: state sentinel {m['sentinel']}")
        # A model OptiScaler created before ReShade loaded is adopted; one created later is owned.
        if addon and not (m["hooked"] and m["created"]):
            problems.append(f"{name}: the add-on did not take OptiScaler's NR ({m['created'] or 'no feature 18'})")
        if addon and mode == 2 and not m["warped"]:
            problems.append(f"{name}: no warped evaluate")
        if addon and mode == 2 and warp == 1 and not m["compute_ready"]:
            problems.append(f"{name}: compute path not ready")
        if temporal and m["temporal_running"] != temporal:
            problems.append(f"{name}: temporal {m['temporal_running']} instead of {temporal}")
    pairs = [("off", "control", 0.5), ("pixel_t0", "compute_t0", 0.35),
             ("d3d11_off", "d3d11_control", 0.5), ("d3d11_pixel_t0", "d3d11_compute_t0", 0.35)]
    for a, b, limit in pairs:
        if a in results and b in results:
            for frame in FRAMES:
                if mad(root / a, root / b, frame) > limit:
                    problems.append(f"{a}/{b} frame {frame}: MAD above {limit}")
    if "pixel_t0" in results and "pixel_t0_repeat" in results:
        for frame in FRAMES:
            if sha256(root / "pixel_t0" / f"dump_{frame}.bmp") != \
                    sha256(root / "pixel_t0_repeat" / f"dump_{frame}.bmp"):
                problems.append(f"frame {frame}: warped runs are not deterministic")
    return problems


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--optiscaler", type=Path, required=True, help="public OptiScaler.dll")
    parser.add_argument("--output-root", type=Path, required=True)
    parser.add_argument("--candidate", default="public_optiscaler")
    parser.add_argument("--cases", nargs="*", default=list(CASES))
    args = parser.parse_args()
    root = args.output_root / args.candidate
    root.mkdir(parents=True, exist_ok=True)
    results = {}
    for name in args.cases:
        if (root / name / "manifest.json").is_file():
            results[name] = json.loads((root / name / "manifest.json").read_text(encoding="utf-8"))
            continue
        results[name] = capture(root / name, args.optiscaler, CASES[name], EXTRA.get(name, ("rr", [])))
        print(f"{name}: exit {results[name]['exit']}, created '{results[name]['created']}', "
              f"warped {results[name]['warped']}, sentinel {results[name]['sentinel']}", flush=True)
    frames = {f"{a}/{b}/{f}": round(mad(root / a, root / b, f), 3)
              for a, b in (("off", "control"), ("pixel_t0", "control"), ("pixel_t0", "compute_t0"),
                           ("pixel_t1", "pixel_t0"), ("compute_t3", "compute_t0"),
                           ("d3d11_off", "d3d11_control"), ("d3d11_pixel_t0", "d3d11_control"),
                           ("d3d11_pixel_t0", "d3d11_compute_t0"),
                           ("d3d11_compute_t1", "d3d11_compute_t0"),
                           ("presr_pixel_t0", "presr_control"),
                           ("own_spatial_pixel_t0", "own_spatial_control"),
                           ("own_spatial_control", "control"))
              for f in FRAMES if a in results and b in results}
    problems = verdict(root, results)
    missing = [name for name in args.cases if name not in results]
    summary = {"result": "PASS" if not problems and not missing else "FAIL", "problems": problems,
               "background_mode": {name: m["background_fallback_reason"]
                                   for name, m in results.items() if CASES[name][3] == 3},
               "mad_rgb8": frames, "optiscaler_sha256": sha256(args.optiscaler / "OptiScaler.dll" if args.optiscaler.is_dir() else args.optiscaler)}
    (root / "summary.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    main()
