"""Compare captured NGX blocks without discarding numeric values or raw logs."""
from pathlib import Path

DIMENSIONS = {"DLSSNR.Width", "DLSSNR.Height"}
RESOURCES = {"DLSSNR.Color", "DLSSNR.Depth", "DLSSNR.MVec", "DLSSNR.Output"}


def read_blocks(path):
    blocks = []
    current = None
    for line in Path(path).read_text(encoding="utf-8-sig").splitlines():
        if line.startswith("[ngx audit] begin "):
            if current is not None:
                raise ValueError(f"Nested audit block: {path}")
            current = (line.split()[-1], {})
        elif line == "[ngx audit] end":
            if current is None:
                raise ValueError(f"Unexpected audit end: {path}")
            blocks.append(current)
            current = None
        elif line.startswith("[ngx audit] "):
            if current is None:
                raise ValueError(f"Entry outside audit block: {path}")
            key, setter, value = line[len("[ngx audit] "):].split(" ", 2)
            if key == "PeripheralWarp.FloatProbe":
                continue
            if key in current[1]:
                raise ValueError(f"Duplicate audit key: {key}")
            current[1][key] = (setter, value)
    if current is not None or not blocks:
        raise ValueError(f"Incomplete or empty audit: {path}")
    return blocks


def equivalent(key, left, right, off=False):
    if left == right:
        return True
    lt, lv = left
    rt, rv = right
    # Resource addresses/setter slots are process-local; roles/descriptors are not.
    if key in RESOURCES and {lt, rt} <= {"d3d12", "pointer"}:
        return lv == rv and lv.startswith("resource ")
    # Owner-approved Off exception, including dimensions retained after Create.
    # Reject negative/overflow sizes and every value difference; never ignore keys.
    if off and key in DIMENSIONS and {lt, rt} == {"int32", "uint32"}:
        return lv == rv and lv.isdecimal() and 0 <= int(lv) <= 0x7fffffff
    return False


def compare(left, right, off=False):
    a, b = read_blocks(left), read_blocks(right)
    if len(a) != len(b):
        raise ValueError(f"Audit block counts differ: {len(a)} / {len(b)}")
    exceptions = 0
    for index, ((op_a, keys_a), (op_b, keys_b)) in enumerate(zip(a, b)):
        if op_a != op_b or keys_a.keys() != keys_b.keys():
            raise ValueError(f"Audit operations/keys differ at block {index}")
        for key in keys_a:
            if not equivalent(key, keys_a[key], keys_b[key], off):
                raise ValueError(f"Audit block {index}, {key}: {keys_a[key]} != {keys_b[key]}")
            if key in DIMENSIONS and keys_a[key] != keys_b[key]:
                exceptions += 1
    return {"blocks": len(a), "equal": True, "off_dimension_type_exceptions": exceptions}
