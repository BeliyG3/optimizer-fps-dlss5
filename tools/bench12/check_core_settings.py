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
    if len(rows) != 57:
        raise ValueError(f'Expected 57 schema rows, found {len(rows)}')
    if rows[45] != ('MenuMode', 'I', '0'):
        raise ValueError(f'Expected schema row 45 to be MenuMode with default 0, found {rows[45]}')
    if rows[46] != ('TemporalGrid', 'I', '1'):
        raise ValueError(f'Expected schema row 46 to be TemporalGrid with default 1, found {rows[46]}')
    if rows[47] != ('TemporalSkyField', 'I', '1'):
        raise ValueError(f'Expected schema row 47 to be TemporalSkyField with default 1, found {rows[47]}')
    if rows[48] != ('DebugTemporalDumpAcc', 'I', '0'):
        raise ValueError(f'Expected schema row 48 to be DebugTemporalDumpAcc with default 0, found {rows[48]}')
    if rows[49] != ('DebugTemporalMarkerRule', 'I', '0'):
        raise ValueError(f'Expected schema row 49 to be DebugTemporalMarkerRule with default 0, found {rows[49]}')
    if rows[50] != ('DebugTemporalMarkerNeighbours', 'I', '0'):
        raise ValueError(f'Expected schema row 50 to be DebugTemporalMarkerNeighbours with default 0, found {rows[50]}')
    if rows[51] != ('TemporalModelMarker', 'I', '2'):
        raise ValueError(f'Expected schema row 51 to be TemporalModelMarker with default 2, found {rows[51]}')
    if rows[52] != ('DebugTemporalCarryProbe', 'I', '0'):
        raise ValueError(f'Expected schema row 52 to be DebugTemporalCarryProbe with default 0, found {rows[52]}')
    if rows[53] != ('DebugTemporalDumpAccFrom', 'I', '0'):
        raise ValueError(f'Expected schema row 53 to be DebugTemporalDumpAccFrom with default 0, found {rows[53]}')
    if rows[54] != ('DebugTemporalChainRule', 'I', '0'):
        raise ValueError(f'Expected schema row 54 to be DebugTemporalChainRule with default 0, found {rows[54]}')
    if rows[55] != ('DebugTemporalCarryChainRule', 'I', '0'):
        raise ValueError(f'Expected schema row 55 to be DebugTemporalCarryChainRule with default 0, found {rows[55]}')
    if rows[56] != ('TemporalChainRule', 'I', '1'):
        raise ValueError(f'Expected schema row 56 to be TemporalChainRule with default 1, found {rows[56]}')
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
