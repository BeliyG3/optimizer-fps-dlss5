"""Compare logged ABI settings with the reference INI and schema defaults."""
import argparse
import configparser
from pathlib import Path
import re
import struct


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--log', required=True, type=Path)
    parser.add_argument('--ini', required=True, type=Path)
    parser.add_argument('--warp-path', type=int, choices=(0, 1, 2), default=0)
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[2]
    schema = (repo / 'core/settings/schema.cpp').read_text(encoding='utf-8')
    rows = re.findall(
        r'Row\(OFPS_SET_\w+,\s*"(\w+)".*?OFPS_TYPE_\w+,\s*[^,]+,\s*[^,]+,\s*([IF])\(([^)]+)\)',
        schema, re.S)
    if len(rows) != 45:
        raise ValueError(f'Expected 45 schema rows, found {len(rows)}')
    ini = configparser.ConfigParser(strict=False, interpolation=None)
    ini.read(args.ini, encoding='utf-8-sig')
    reference = ini['OptimizerFPS'] if ini.has_section('OptimizerFPS') else ini['PeripheralWarp']
    logged = dict((int(i), int(bits)) for i, bits in re.findall(
        r'\[core settings\] id=(\d+) bits=(-?\d+)', args.log.read_text(encoding='utf-8')))
    for setting_id, (key, kind, default) in enumerate(rows):
        value = str(args.warp_path) if key == 'DebugWarpPath' else reference.get(key, default.rstrip('f'))
        expected = (struct.unpack('<i', struct.pack('<f', float(value)))[0]
                    if kind == 'F' else int(value))
        if logged.get(setting_id) != expected:
            raise ValueError(f'{key} (id {setting_id}): expected bits {expected}, got {logged.get(setting_id)}')
    print(f'Core effective settings: {len(rows)} schema values match {args.ini.name}')


if __name__ == '__main__':
    main()
