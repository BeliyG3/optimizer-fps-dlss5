import pathlib
import re
import sys
root = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else ".").resolve()
core = root / "core"
if not core.is_dir():
    sys.exit("core is missing")
bad = []
for path in core.rglob("*"):
    if path.suffix not in {".h", ".cpp", ".inl"}:
        continue
    for n, line in enumerate(path.read_text(encoding="utf-8-sig").splitlines(), 1):
        match = re.match(r'\s*#\s*include\s*["<]([^">]+)', line)
        if match and re.search(r"ngx_bridge|reshade|detours|d3d11_adapter|model_host_ngx|ngx_hook_api|shell_host|ngx_params|ngx_forwarder_calls|\.\./", match[1], re.I):
            bad.append(f"{path.relative_to(root)}:{n}: {match[1]}")
print("\n".join(bad) if bad else "core include boundary ok")
sys.exit(bool(bad))
