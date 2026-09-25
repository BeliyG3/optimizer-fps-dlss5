"""Check the shared manifest against compiled DXBC reflection, without a GPU."""
from pathlib import Path
import re
import subprocess
import sys

manifest, binaries, fxc = map(Path, sys.argv[1:])
rows = re.findall(r"PW_TEMPORAL_PASS\((\w+), \w+, (0x[0-9a-f]+), (\d+), \w+\)", manifest.read_text())
assert rows, "Empty temporal manifest"
stats_masks = [int(mask, 16) for name, mask, _ in rows if name == "Stats"]
required_stats_registers = {0, 3, 4, 5, 6, 7, 11}
assert stats_masks == [sum(1 << slot for slot in required_stats_registers)], "Stats tile SRV contract"
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
for name in ("Refine", "FlowLuma"):
    mask = int(next(mask for pass_name, mask, _ in rows if pass_name == name), 16) | (1 << 5)
    result = subprocess.run([str(fxc), "/dumpbin", str(binaries / f"temporal_{name}Model_cs.dxbc")],
                            capture_output=True, text=True, check=True)
    slots = re.findall(r"^dcl_resource_texture2d .* t(\d+)$", result.stdout, re.M)
    assert sum(1 << int(slot) for slot in slots) == mask, f"{name}Model: SRV mask differs"
print(f"Verified {len(rows)} temporal DXBC register layouts and dispatch groups")
