# Configuration

For the ReShade add-on, settings live in the `[OptimizerFPS]` section of the
host's `ReShade.ini`. In a 32-bit game, this is the 64-bit host's ini; the game's
remote tab controls that host. Use the overlay when possible. For a manual edit,
close the game and host first, then restart them after saving the ini.

```ini
[OptimizerFPS]
Mode=2
CenterX=80
CenterY=80
WorkX=90
WorkY=90
CrashGuard=1
DebugWarpPath=0
```

`Mode=0` is Off, `1` is Uniform, and `2` is Peripheral. `CrashGuard=0` disables the marker that
keeps warping off after an early crash. `Passive=1` is a diagnostic setting: the
add-on registers without hooks, events, or a tab. Remove it after the test.
`DebugWarpPath=0` selects the available path automatically, `1` forces compute,
and `2` forces pixel. Start with `0`; forcing an unsupported path can leave the
frame untouched. The Diagnostics tree in the tab and `ReShade.log` explain why a
frame was passed through. `DebugTiming` enables timing queries; other `Debug*`
keys are for specific investigations and may need a restart.

An older `[PeripheralWarp]` section is only an input for migration. `Update`
copies known keys to `[OptimizerFPS]` when the current section is absent. If
both exist, the current section wins. See [INSTALL.md](INSTALL.md).

Percentages are per axis.

| Field | Meaning |
|---|---|
| `mode` | `Off`, `Uniform`, or `Peripheral` |
| `centerPercent` | width of the center band kept at native 1:1 density |
| `workShiftXPercent` / `workShiftYPercent` (v2) | signed shift of the raw Work rectangle (outer contour) along the axis, percent of the axis, with the center band fixed: work pixels move from one periphery to the other. Range from `WorkShiftLimitsPercentV2` |
| `centerOffsetXPercent` / `centerOffsetYPercent` (v2) | signed offset of that band from the frame center, percent of the axis; Peripheral only. Work stays the same size: the wider periphery is compressed harder, the narrower one never below 1:1. Limit: `|offset| <= (100 - center) / 2 - 0.5` |
| `workPercent` | total work extent relative to native |
| `globalScale` (v3) | uniform scale applied to both axes inside the same Pack mapping |
| `colorFilter` | bilinear or adaptive four-tap peripheral prefilter |
| `ConfigFlagExtendMotionAtEdge` | linearly extend motion endpoints beyond the screen instead of clamping |
| `ConfigFlagInputConfidenceValid` | require and conservatively filter confidence |

For ABI v3 Peripheral mode, each axis must satisfy:

```text
1 <= Center < Work <= 100
25 <= Work
25 <= GlobalScale * Work / 100
```

ABI v1/v2 retain their original validation and binary layout. New integrations use v3 for aggressive Work and Global scale.

The default `80 -> 90` mapping with Global 100% at 3840×2160 yields 3456×1944. Effective dimensions are rounded up and then to an even integer before shader constants are calculated:

```text
effectiveWidth  = even(nativeWidth  * globalScale * rawWorkX)
effectiveHeight = even(nativeHeight * globalScale * rawWorkY)
```

The unchanged center area is `0.8 × 0.8 = 64%` of the screen. The work texture contains `0.9 × 0.9 = 81%` of the native pixels, so the theoretical pixel reduction for the middle renderer is 19%. Pack, Unpack, optical-flow preparation, frame generation, and game rendering remain additional costs.

The old quality guard limited local peripheral compression to 2x and produced the visible 50.5% minimum. V3 replaces that hard rejection with an aggressive-compression diagnostic. `compression < 0.5` may visibly alias near the edge even with the automatic prefilter.

Use Center/Work `100/100` and Global 100% for a strict identity extent. Rejected configurations return a `ofps::sdk::Status`; callers should keep the last valid layout or bypass the module.

## Suggested presets

| Name | Mode | Center X/Y | Work X/Y | Global | Notes |
|---|---|---:|---:|---:|---|
| Baseline | Off | - | - | 100% | Compare against native-resolution NR |
| Quality | Peripheral | 80% | 90% | 100% | 3456x1944 from a 4K source |
| Balanced | Peripheral | 80% | 90% | 85% | 2938x1654 from a 4K source after upward even rounding |
| Aggressive | Peripheral | 20% | 40% | 100% | Valid ABI v3 experiment; expect edge aliasing |

The preset names are documentation conveniences, not serialized enum values. Always record the
actual five numeric values in a benchmark report.

`Show uncompressed center` (cyan) and `Show Work boundary` (orange) are diagnostics, but they are
persisted like the rest of the layout (`ShowCenterOutline` / `ShowWorkOutline` in `[OptimizerFPS]`)
so they survive ReShade re-creating its runtime. The Work boundary represents raw Work before Global
scale. Both are hidden outside Peripheral mode and change neither layout generation nor temporal
reset state.

The add-on's own ini keys, including these and the temporal cadence, are listed in
[RESHADE_ADDON.md](RESHADE_ADDON.md).
