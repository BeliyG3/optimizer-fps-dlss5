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
`DebugTemporalDumpAcc=1` (temporal mode 1) writes each full pass's model motion and motion chain to `ofps_acc_<eval>.bin` beside the game's executable (`core/frame/acc_dump.h`).
`DebugTemporalMarkerRule` (0..4, default 0 = the rule `TemporalModelMarker` selects) narrows where the model is told a pixel has no history: 1 only off-screen, non-finite and depth failures mark; 2 the colour test marks only where the depth mismatch is at least the tolerance; 3 and 4 are 1 and 2 marking only failures with at least 12 of the 25 pixels of their 5x5 neighbourhood failing too (`PwTMarkerReason` in `core/shaders/temporal_residual.hlsli`).
`DebugTemporalMarkerNeighbours` (0..25, default 0 = 12) is that threshold for rules 3 and 4: how many of the 25 pixels, the failing one included, must fail.
Both override `TemporalModelMarker` when above 0: its `1` is rule 0, its `2` rule 4 with 8 neighbours, its `0` the plain chain (as `DebugTemporalNoModelMotion=1`).
A debug rule replaces the setting's neighbour count too: `DebugTemporalMarkerRule=4` under `TemporalModelMarker=2` uses 12 (the `DebugTemporalMarkerNeighbours` default), not 8; and a debug rule above 0 turns the markers on even under `TemporalModelMarker=0`.
`DebugTemporalCarryProbe` (0..7, default 0; for diagnostics only) adds bits: 1 = temporal mode 1 takes the host's motion as the finished chain (in the project's test scene, that motion is the offset to the last full pass: no accumulation, link test, Refine or expectation; other modes ignore it and log once); 2 = the reprojection accepts every in-frame pixel; 4 = the residual is fetched bilinearly instead of Catmull-Rom.
`DebugTemporalDumpAccFrom` (0..1000000, default 0 = off; for diagnostics only) dumps the chain on every evaluate from that one to 63 evaluates later (with `DebugTemporalDumpAcc=1`, temporal mode 1), carried frames included: their model plane repeats the chain and their reasons are 0; full passes outside the window are not dumped.
`DebugTemporalChainRule` (0..4, default 0; for diagnostics only) decides where the carry's motion chain starts afresh from this frame's vector (`PSAccumulate` in `core/shaders/temporal_residual.hlsli`; only where the chain's end is in the frame and the expected depth is known, as before): 0 = a depth mismatch above twice the tolerance or a colour change (the classic rule);
1 = never; 2 = a depth mismatch above twice the tolerance, or a colour change where the mismatch is at least the tolerance; 3 = rule 2, and a failing pixel restarts only where at least 8 of the 25 pixels of its 5x5 neighbourhood fail rule 2 too (without the `temporal_LinkFail_cs.dxbc` pass rule 3 runs as rule 2 and logs once); 4 = depth only. The background mode's pending chain keeps no link test; the host-motion chain (`DebugTemporalCarryProbe` value 1) overrides every rule.
`DebugTemporalCarryChainRule` (0..6, default 0; for diagnostics only) adds a second chain, the carry chain, that restarts by that rule (1..4: the values of `DebugTemporalChainRule`), while `DebugTemporalChainRule` keeps driving the main chain.
Two more tell a change of a whole area (a reflection, a light) from the speckled changes of swaying foliage: 5 = a depth mismatch above twice the tolerance, or a colour change where at least 18 of the 25 pixels of its 5x5 neighbourhood change colour too, whatever the depth;
6 = rule 5, and a restart where last frame's `Refine` moved the main chain one way over the 5x5 neighbourhood (a mean correction of at least 0.5 native pixels and at least 0.7 of the mean correction's length: the picture moved against the game's motion).
The thresholds are `PW_TEMPORAL_COHERENT_*` in `core/shaders/temporal_layout.h`. Both chains run in one pass each (`AccumulatePair`, which also takes the 5x5 votes, and `RefinePair`, `core/shaders/temporal_chain_pair.hlsli`); without those binaries there is no carry chain (logged once).
With `DebugTemporalDumpAcc=1` and a carry rule the dump uses format version 3 (`OFAC` v3): a fourth plane holds the chain the carried frames reproject along (the main chain on a full pass, which records no carry chain).
The carry chain lives on the main machine only and feeds the carried frames it shows (the reprojection, and so its cells and compose; the background mode's carried frames included). The hidden spread stages keep the main chain alone: their carried frame is the next stage's model input. A full pass records no carry chain.
The main chain feeds everything a full pass hands the model (its vectors and no-history markers), the history link, the residual blend, the statistics and the chain dump's chain plane (`DebugTemporalDumpAcc`: the model's view).
Both restart on a full pass; where the carry chain has none of its own (an adopted background pass, an optical-flow seed, the key just turned on) it continues from the main chain. The pending chain is unchanged, and the host-motion chain (`DebugTemporalCarryProbe` value 1) sets both. 0 = no carry chain: nothing extra is created or recorded.
Both keys override `TemporalChainRule` when either is above 0 (`core/temporal/chain_rule.h`): both chains then follow the two keys as before the setting existed, so `DebugTemporalChainRule=3` alone is one chain by rule 3 even under the default. With both at 0 the setting decides: its `1` is main rule 0 with carry rule 5, its `0` main rule 0 without a carry chain.

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
| `colorFilter` | `Bilinear` (0), `AdaptiveFourTap` (1, soft pre-filter and cubic unpack), `DetailTransfer` (2, full-size frame plus the model's edit where shrunk) or `DetailTransferDepth` (3, the same with the edit weighted by depth); 2/3 run in the Optimizer FPS core's compute Unpack, SDK adapters treat them as `AdaptiveFourTap` |
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

With a temporal mode, the layout also decides whether carried frames are reprojected at the model's resolution
(`TemporalGrid=1`, the default): only when the model gets about 80 % or less of the native resolution on both axes,
for example Peripheral with Global scale at 80 % or less. The Quality and Balanced presets above stay at native
resolution. `TemporalGrid=0` keeps carried frames exactly as before; see
[TEMPORAL_MODES.md](TEMPORAL_MODES.md#carried-frames-at-the-models-resolution).

Carried frames also give the sky a smooth estimate of the model's sky edit (`TemporalSkyField=1`, the default), so
fast turns leave no pale patches on it. The sky is what lies exactly at the far plane of the game's depth buffer; a
host that hands over linear depth should switch it off (`TemporalSkyField=0`, carried frames exactly as before); see
[TEMPORAL_MODES.md](TEMPORAL_MODES.md#smooth-sky-in-carried-frames).

Full passes tell the model where a pixel has no history with the narrow rule (`TemporalModelMarker=2`, the default),
which keeps the history of swaying foliage and still marks real disocclusions. `TemporalModelMarker=1` is the rule
before 2026.10.2 and `0` turns the markers off; see
[TEMPORAL_MODES.md](TEMPORAL_MODES.md#model-motion-markers).

Carried frames follow a chain of their own that keeps following a surface through scattered colour flicker
(`TemporalChainRule=1`, Coherent, the default), so moving shots smear and flicker less on foliage; the model's own
frames are unchanged. `TemporalChainRule=0` (Classic) is one chain as in earlier releases: use it if moving shots
look worse than before, or to compare; see [TEMPORAL_MODES.md](TEMPORAL_MODES.md#carry-chain).
On a host that seeds the chain from optical flow every frame, Coherent has practically no effect (harmless): both
chains start from the seeded chain each frame, so the carry rule only decides that frame's last link.

The add-on's own ini keys, including these and the temporal cadence, are listed in
[RESHADE_ADDON.md](RESHADE_ADDON.md).
