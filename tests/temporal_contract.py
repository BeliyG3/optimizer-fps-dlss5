"""Check the shared manifest against compiled DXBC reflection, without a GPU."""
from pathlib import Path
import re
import subprocess
import sys

manifest, binaries, fxc = map(Path, sys.argv[1:])
rows = re.findall(r"PW_TEMPORAL_PASS\((\w+), \w+, (0x[0-9a-f]+), (\d+), \w+\)", manifest.read_text())
assert rows, "Empty temporal manifest"
for name, mask, outputs in rows:
    result = subprocess.run([str(fxc), "/dumpbin", str(binaries / f"temporal_{name}_cs.dxbc")],
                            capture_output=True, text=True, check=True)
    reflection = result.stdout
    slots = re.findall(r"^dcl_resource_texture2d .* t(\d+)$", reflection, re.M)
    actual = sum(1 << int(slot) for slot in slots)
    assert actual == int(mask, 16), f"{name}: SRV mask {actual:#x} differs from {mask}"
    uavs = re.findall(r"^dcl_uav_typed_texture2d .* u(\d+)$", reflection, re.M)
    assert sorted(map(int, uavs)) == list(range(int(outputs))), f"{name}: output layout differs"
    assert "dcl_thread_group 16, 8, 1" in reflection, f"{name}: dispatch group differs"
    # FXC trims trailing unused constant registers, but reflection retains declared member offsets.
    assert "PwTSmooth;                  // Offset:  128 Size:    16" in reflection, f"{name}: b0 layout differs"
    assert "dcl_constantbuffer CB1[" in reflection, f"{name}: missing dispatch constants"
print(f"Verified {len(rows)} temporal DXBC register layouts and dispatch groups")
