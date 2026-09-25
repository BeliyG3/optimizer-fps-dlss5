"""Checks replacement rules and byte-preserving IO in temporary files: no repository, no git, no map file.
    python tools/rename/test_rename.py
"""
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import rename  # noqa: E402
def check(kind, old, new, text, expected):
    compiled = rename.compile_rule(rename.Rule(kind, old, new))
    got, count = rename.apply_rule(text, compiled)
    assert got == expected, f"{kind} {old!r}: {got!r} != {expected!r}"
    return count
assert check("word", "pw::", "ofps::sdk::",
             "pw::ConfigV2 c; pwngx::Log(); pw_ngx::Poll(); pwhook::X y;",
             "ofps::sdk::ConfigV2 c; pwngx::Log(); pw_ngx::Poll(); pwhook::X y;") == 1
assert check("word", "namespace pw", "namespace ofps::sdk",
             "namespace pw {\n} // namespace pw\nnamespace pw_addon {\n",
             "namespace ofps::sdk {\n} // namespace ofps::sdk\nnamespace pw_addon {\n") == 2
assert check("word", "PW_SDK_VERSION", "OFPS_SDK_VERSION",
             "@PW_SDK_VERSION@ PW_SDK_VERSION_STRING (PW_SDK_VERSION)",
             "@OFPS_SDK_VERSION@ PW_SDK_VERSION_STRING (OFPS_SDK_VERSION)") == 2
assert check("word", "PwOptions", "OfpsOptions", "PwOptions.x PwOptionsX", "OfpsOptions.x PwOptionsX") == 1
assert check("word", "peripheral_warp_core", "ofps_sdk_core",
             "peripheral_warp_core.lib peripheral_warp_core_extra", "ofps_sdk_core.lib peripheral_warp_core_extra") == 1
assert check("word", "peripheral_warp_d3d11", "ofps_sdk_d3d11",
             "peripheral_warp_d3d11 peripheral_warp_d3d11_smoke", "ofps_sdk_d3d11 peripheral_warp_d3d11_smoke") == 1
assert check("word", "PW_X", r"OFPS_\1$", "PW_X", r"OFPS_\1$") == 1
assert check("text", "peripheral_warp/", "optimizer_fps/",
             '#include "peripheral_warp/types.h"', '#include "optimizer_fps/types.h"') == 1
assert check("regex", r"\b_pw_([A-Za-z0-9_]+)", r"_ofps_\1",
             "set(_pw_line x) ${_pw_outputs}", "set(_ofps_line x) ${_ofps_outputs}") == 2
line = '/I "${PROJECT_SOURCE_DIR}/shaders" /Fo'
once, n1 = rename.apply_rule(line, rename.compile_rule(rename.Rule(
    "regex", r'(?<!sdk/shaders" )/I "\$\{PROJECT_SOURCE_DIR\}/shaders"',
    '/I "${PROJECT_SOURCE_DIR}/sdk/shaders" /I "${PROJECT_SOURCE_DIR}/shaders"')))
twice, n2 = rename.apply_rule(once, rename.compile_rule(rename.Rule(
    "regex", r'(?<!sdk/shaders" )/I "\$\{PROJECT_SOURCE_DIR\}/shaders"',
    '/I "${PROJECT_SOURCE_DIR}/sdk/shaders" /I "${PROJECT_SOURCE_DIR}/shaders"')))
assert once == '/I "${PROJECT_SOURCE_DIR}/sdk/shaders" /I "${PROJECT_SOURCE_DIR}/shaders" /Fo' and (n1, n2) == (1, 0)
assert twice == once
# Exercise the real byte-reading/writing path, including BOM and line endings.
import tempfile
with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    rule = rename.compile_rule(rename.Rule("word", "PW_X", "OFPS_X"))
    for bom in (b"", b"\xef\xbb\xbf"):
        for newline in (b"\n", b"\r\n"):
            sample = root / "sample.cpp"
            before = bom + b"PW_X" + newline + b"PW_X" + newline
            expected = bom + b"OFPS_X" + newline + b"OFPS_X" + newline
            sample.write_bytes(before)
            rename.rewrite_files(root, [rule], apply=False)
            assert sample.read_bytes() == before
            rename.rewrite_files(root, [rule], apply=True)
            assert sample.read_bytes() == expected
            rename.rewrite_files(root, [rule], apply=True)
            assert sample.read_bytes() == expected
assert rename.is_candidate(Path("adapters/reshade/ngx/ngx_common.h"))
assert rename.is_candidate(Path("CMakePresets.json"))
assert rename.is_candidate(Path("sdk/adapters/d3d12/CMakeLists.txt"))
assert rename.is_candidate(Path("tools/Package-Release.ps1"))
assert rename.is_candidate(Path("docs/API.md"))
assert not rename.is_candidate(Path("tools/bench12/run_addon/final/runs.json"))
assert not rename.is_candidate(Path("tools/bench12/shaders/reservoir.hlsli"))
assert not rename.is_candidate(Path("docs/superpowers/specs/2026-09-22-one-core-design.md"))
assert not rename.is_candidate(Path("docs/dev/CLEANUP_26_26.md"))
assert not rename.is_candidate(Path("CHANGELOG.md"))
assert not rename.is_candidate(Path("tools/rename/test_rename.py"))
assert not rename.is_candidate(Path("out/build/x64/CMakeCache.txt"))
assert rename.is_candidate(Path("shaders/temporal_passes.def"))  # .def is scanned (the map leaves PW_TEMPORAL_PASS alone)
assert not rename.is_candidate(Path("tools/bench12/pw_bench12.exe"))
print("rename.py rules ok")
