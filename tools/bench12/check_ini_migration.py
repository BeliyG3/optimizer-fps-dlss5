"""Exercise ReShade.ini migration through the real add-on and ReShade cache."""

import argparse
import configparser
import os
from pathlib import Path
import re
import stat
import subprocess


CASES = ("legacy_2615", "legacy_2628", "new_wins", "both_sections",
         "empty_new", "no_section", "withdrawn_mode", "write_denied")
SHELL_KEYS = {"Passive", "FloatingWindow", "TraceExit", "DebugLayer", "CrashGuard"}


def sections(data: bytes) -> configparser.ConfigParser:
    parser = configparser.ConfigParser(interpolation=None, strict=False)
    parser.optionxform = str
    parser.read_string(data.decode("utf-8-sig"))
    return parser


def schema_keys(root: Path) -> set[str]:
    source = (root / "core/settings/schema.cpp").read_text(encoding="utf-8")
    keys = set(re.findall(r'Row\(OFPS_SET_\w+,\s*"([^"]+)"', source))
    if len(keys) != 45:
        raise AssertionError(f"Expected 45 schema keys, got {len(keys)}")
    return keys | SHELL_KEYS


def assert_preserved(before: configparser.ConfigParser,
                     after: configparser.ConfigParser, name: str) -> None:
    for section in ("PeripheralWarpDLSS", "RenoDX.DLSS5", "DLSS5Host"):
        if before.has_section(section):
            if not after.has_section(section) or dict(before[section]) != dict(after[section]):
                raise AssertionError(f"{name}: [{section}] key/value set changed")
    if before.has_option("ADDON", "DisabledAddons"):
        if before.get("ADDON", "DisabledAddons") != after.get("ADDON", "DisabledAddons"):
            raise AssertionError(f"{name}: DisabledAddons changed")


def run_host(runtime: Path, root: Path, output: Path, label: str) -> str:
    (runtime / "optimizer-fps-dlss5.session").unlink(missing_ok=True)
    (runtime / "ReShade.log").unlink(missing_ok=True)
    command = [str(runtime / "pw_bench12.exe"), "240", "--gltf",
               str(root / "tools/bench/assets/lab_scene.glb"), "--upscaler", "rr",
               "--sun-dir", "0.45,-0.77,0.45", "--sun-strength", "1500",
               "--exposure", "0.22", "--haze", "0.008", "--fov", "62"]
    result = subprocess.run(command, cwd=runtime, capture_output=True, text=True,
                            errors="replace", timeout=240, check=False)
    (output / f"{label}.stdout.txt").write_text(result.stdout + result.stderr,
                                                 encoding="utf-8")
    log_path = runtime / "ReShade.log"
    log = log_path.read_text(encoding="utf-8", errors="replace") if log_path.exists() else ""
    (output / f"{label}.ReShade.log").write_text(log, encoding="utf-8")
    if result.returncode or "device ok" not in result.stdout or not log:
        raise AssertionError(f"{label}: host failed ({result.returncode}); see {output}")
    return log


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runtime", type=Path,
                        default=Path(__file__).parent / "run_addon")
    parser.add_argument("--build", type=Path,
                        default=Path(__file__).resolve().parents[2] / "out/build/x64")
    parser.add_argument("--case", choices=CASES)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    runtime = args.runtime.resolve()
    output = root / "out/ini-migration-integration"
    output.mkdir(parents=True, exist_ok=True)
    fixtures = root / "tests/fixtures/reshade_ini"
    known = schema_keys(root)
    sources = {
        "optimizer-fps-dlss5.addon64": args.build / "hosts/reshade/Release/optimizer-fps-dlss5.addon64",
        "optimizer-fps-dlss5-core.dll": args.build / "core/Release/optimizer-fps-dlss5-core.dll",
    }
    originals = {name: (runtime / name).read_bytes() for name in sources}
    ini_path = runtime / "ReShade.ini"
    original_ini = ini_path.read_bytes()
    try:
        for name, source in sources.items():
            (runtime / name).write_bytes(source.read_bytes())
        for name in (args.case,) if args.case else CASES:
            fixture_name = "legacy_2615" if name in ("withdrawn_mode", "write_denied") else name
            fixture = (fixtures / f"{fixture_name}.ini").read_bytes() if name != "no_section" else (
                b"[ADDON]\r\nDisabledAddons=\r\n[PeripheralWarpDLSS]\r\nMode=2\r\n")
            if name == "withdrawn_mode":
                fixture = fixture.replace(b"TemporalMode=1", b"TemporalMode=2", 1)
            before = sections(fixture)
            ini_path.write_bytes(fixture)
            if name == "write_denied":
                os.chmod(ini_path, stat.S_IREAD)
                try:
                    run_host(runtime, root, output, f"{name}.denied")
                finally:
                    os.chmod(ini_path, stat.S_IREAD | stat.S_IWRITE)
                if ini_path.read_bytes() != fixture:
                    raise AssertionError("write_denied: flush changed read-only source")
                recovered_log = run_host(runtime, root, output, f"{name}.recovered")
                recovered = sections(ini_path.read_bytes())
                assert_preserved(before, recovered, name)
                if "migrated " not in recovered_log or not recovered.has_section("OptimizerFPS"):
                    raise AssertionError("write_denied: writable restart did not migrate")
                print("PASS write_denied: flush rejected; writable restart migrated")
                continue
            first_log = run_host(runtime, root, output, f"{name}.first")
            first_bytes = ini_path.read_bytes()
            (output / f"{name}.first.ini").write_bytes(first_bytes)
            first = sections(first_bytes)
            assert_preserved(before, first, name)
            if before.has_section("PeripheralWarp") and (
                    dict(first["PeripheralWarp"]) != dict(before["PeripheralWarp"])):
                raise AssertionError(f"{name}: old section changed")
            if name.startswith("legacy_") or name == "withdrawn_mode":
                if "migrated " not in first_log:
                    raise AssertionError(f"{name}: no migration log")
                expected = {key: value for key, value in before["PeripheralWarp"].items()
                            if key in known}
                if not first.has_section("OptimizerFPS") or dict(first["OptimizerFPS"]) != expected:
                    raise AssertionError(f"{name}: migrated keys differ from present known keys")
            elif "migrated " in first_log:
                raise AssertionError(f"{name}: unexpected migration")
            if before.has_section("OptimizerFPS") and (
                    dict(first["OptimizerFPS"]) != dict(before["OptimizerFPS"])):
                raise AssertionError(f"{name}: existing new section changed")
            if name == "empty_new" and dict(first["OptimizerFPS"]):
                raise AssertionError("empty_new: legacy fallback filled empty new section")
            if name == "no_section" and first.has_section("OptimizerFPS"):
                raise AssertionError("no_section: created a section without an edit")
            if name == "both_sections" and first.get("OptimizerFPS", "Mode") != "0":
                raise AssertionError("both_sections: new Mode lost priority")
            if name == "no_section" and "model 1728x972 (warped)" not in first_log:
                raise AssertionError("no_section: schema default layout was not applied")
            if name == "withdrawn_mode" and "temporal mode 1 running" not in first_log:
                raise AssertionError("withdrawn_mode: core did not normalize mode 2 to 1")
            second_log = run_host(runtime, root, output, f"{name}.second")
            second_bytes = ini_path.read_bytes()
            (output / f"{name}.second.ini").write_bytes(second_bytes)
            second = sections(second_bytes)
            assert_preserved(before, second, name)
            if before.has_section("PeripheralWarp") and (
                    dict(second["PeripheralWarp"]) != dict(before["PeripheralWarp"])):
                raise AssertionError(f"{name}: restart changed old section")
            if "migrated " in second_log:
                raise AssertionError(f"{name}: migrated twice")
            if before.has_section("OptimizerFPS") and (
                    dict(second["OptimizerFPS"]) != dict(before["OptimizerFPS"])):
                raise AssertionError(f"{name}: restart changed existing new section")
            if (name.startswith("legacy_") or name == "withdrawn_mode") and (
                    dict(second["OptimizerFPS"]) != dict(first["OptimizerFPS"])):
                raise AssertionError(f"{name}: second launch changed migrated keys")
            print(f"PASS {name}: flush and restart")
    finally:
        os.chmod(ini_path, stat.S_IREAD | stat.S_IWRITE)
        ini_path.write_bytes(original_ini)
        for name, data in originals.items():
            (runtime / name).write_bytes(data)


if __name__ == "__main__":
    main()
