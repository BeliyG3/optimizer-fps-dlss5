"""Verify the public ABI 1 headers against the accepted narrow-host baseline."""

from hashlib import sha256
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
FROZEN_SHA256 = {
    "core/api/ofps_core.h": "6AA1DDD489B2206AEA709E095160BE8AFFA7A819E1AB96464091F4DDCE2B4AF1",
    "core/api/ofps_settings_schema.h": "1C42B333073559558FA2AE1050DEE63751B0B31040DFE9F79F9A02A6900B4EFA",
}


def main() -> int:
    failed = False
    for relative_path, expected in FROZEN_SHA256.items():
        actual = sha256((ROOT / relative_path).read_bytes()).hexdigest().upper()
        if actual != expected:
            print(f"FAIL {relative_path}: expected {expected}, got {actual}")
            failed = True
        else:
            print(f"PASS {relative_path}: {actual}")
    return int(failed)


if __name__ == "__main__":
    raise SystemExit(main())
