# pw_bench12 display like Unreal Engine 5: `--tonemap ue-filmic`, `--local-exposure`, `--exposure-metering ue`

The display (both render modes, after the upscaler and NR) can work as UE5's post process does: UE's filmic tone curve,
UE5's local exposure and UE's histogram eye adaptation. Without these options the frame is unchanged (dumps byte-identical
to the build before them). Why they exist and what was measured: RASTER_ACCEPTANCE.md, "Outdoor colours".

| Option | Default | Meaning |
| --- | --- | --- |
| `--tonemap ue-filmic` | `aces` | UE's filmic curve (below). `aces` (the per-channel ACES fit), `neutral` and `none` stay as they were. |
| `--film-slope S`, `--film-toe T`, `--film-shoulder H`, `--film-black-clip B`, `--film-white-clip W` | `0.88`, `0.55`, `0.26`, `0`, `0.04` | UE's Post Process "Film" settings (UE's defaults); `ue-filmic` only. Slope 0.01..2, the rest 0..1. |
| `--local-exposure 0\|1` | `0` | UE5's local exposure (below). |
| `--local-exposure-highlights C`, `--local-exposure-shadows C` | `0.8`, `0.8` | Contrast scales of the local base above and below mid grey, 0..2 (UE: "Highlight/Shadow Contrast Scale", default 1 = no effect). |
| `--local-exposure-detail D` | `1` | Detail strength, 0..4 (UE's default 1). |
| `--local-exposure-blend F` | `0.6` | Share of the wide blur in the base, 0..1 (UE: "Blurred Luminance Blend", 0.6). |
| `--exposure-metering bench\|ue` | `bench` | With `--auto-exposure 1`: `bench` meters the log-average to 0.18 with the highlight cap (README); `ue` as UE (below). |
| `--exposure-min-ev E`, `--exposure-max-ev E` | `0.5`, `20` | `ue` metering: UE's Min/Max EV100 of the total exposure (`--exposure` times the scale), -16..32. |
| `--exposure-curve LOW,HIGH,EV` | `3,6,0.33` | `ue` metering: UE's Exposure Compensation Curve as a ramp: no extra stops up to a metered scene EV100 of LOW, EV stops from HIGH (below). |

All of them are in the menu (Render: "Tone map" with the film sliders, "Local exposure" with its four, "Metering" with the
EV100 bounds under "Auto exposure") and saved in `settings.json` under the same names.

## UE filmic (`shaders/tonemap_ue.hlsli`)

UE's `FilmToneMap` (TonemapCommon.ush): linear sRGB to ACES AP0 for the RRT's glow and red modifier, then to ACEScg (AP1),
4 % desaturation, the curve per channel, 7 % desaturation, back to linear sRGB. The curve is a straight segment of slope
`slope` in log10 through 0.18 -> 0.18, with a logistic toe (`toe`, `black clip`) and shoulder (`shoulder`, `white clip`):
0.18 -> 0.18, 1 -> 0.72, 2 -> 0.86 (the bench's ACES fit: 0.27, 0.80, 0.90, so `ue-filmic` is about 0.6 stop darker in
the mid-tones at the same exposure). Not included: UE's grading, blue correction and gamut expansion. Checks
(`tests_tonemap.cpp`, the shader's own code through `hlsl_compat.h`): the curve's 0.18 and 1.0 points, monotonic, grey
stays grey, AP1 round trip, a pink, a red and a white from shade to 1000x roll towards white without a hue shift over
8 degrees while in range. Far past white a saturated red still drifts to orange (25 degrees at 30x; the ACES fit 13):
the curve is still per channel, in a wider space.

## Local exposure (`shaders/local_exposure.hlsl`, `.hlsli`, `local_exposure.cpp`)

UE5's `LocalExposure`: a bilateral grid of the frame's log2 luminance after the auto exposure, cells of 64 pixels and 32
slices of half a stop from -10 to +6, built each frame after the meter (one group per cell, fixed-point sums in groupshared
memory), blurred 5x5 across cells and 3 across slices. The present pass slices it at the pixel's own luminance (the mean of
its neighbours about as bright: an edge-aware base), mixes in `blend` of a wide blur (the display pyramid's level at most 8
texels wide; its alpha is the mean log luminance) and computes
`local = mid + (base - mid) * contrast + (pixel - base) * detail`, `mid` = log2 0.18, `contrast` = highlights above mid,
shadows below. The colour is scaled by `2^(local - pixel)` before the bloom is added. With 0.8 / 0.8 sunlit and shaded
regions move 20 % closer in stops while their texture (the detail) stays. Cost: two compute passes on a 30x17x32 grid at
1080p, within the noise of the frame time (display stage +0.1 ms on a shared GPU).

## UE metering (`MeterBandAverage`, `UeMeterTarget` in `shaders/display_math.hlsli`)

UE's histogram auto exposure: the mean log2 luminance of the pixels between 10 % and 90 % of the histogram's weight (the
darkest and brightest tenth ignored) is mapped to 0.18, clamped to the Min/Max EV100 (UE's exposure for an EV100 is
`1 / (1.2 * 2^EV100)`, of the total exposure), then `--exposure-ev` on top. There is no highlight cap: the shoulder and
the local exposure keep the highlights. The bench metering's cap held the garden 1.1-1.8 stops below this; the hall,
dark next to its lamps, would rise 2 stops, so `--exposure-min-ev 0.5` (at `--exposure 1.45`: scale at most 0.406) keeps
it where the bench metering had it, the look of the hall unchanged with `ue-filmic` and local exposure.

**Daylight compensation** (`--exposure-curve`, `SceneEv100`, `ExposureCurveBias`): UE's "Exposure Compensation Curve"
adds stops by the scene's metered EV100 (`log2 L + 3` of the band's mean radiance, L = colour / `--exposure`). The ramp
from EV100 3 (nothing) to 6 (+0.33) gives sunlit exteriors (the garden's views meter 6.3-7.6) a third of a stop and the
hall (-0.5) none, so the hall stays on its EV100 bound. A constant `--exposure-ev` would raise the hall as well (it is
applied after the bound, as in UE), and a Reinhard-style auto key depends on absolute luminance the scene is not
calibrated in. Why a third: measured at +0.33 / +0.67 / +1 the white suit's and the pink mannequin's display p95 go
230-235 / 238-239 / 242 (picnic, dancers, suit views), so a third is the most that keeps them at or below ~235.

## The garden as UE5 (the owner's interactive settings)

`--tonemap ue-filmic --local-exposure 1 --auto-exposure 1 --exposure-metering ue` with the defaults above
(`rt_fly_garden6\settings.json`). Measured at 1920x1080 (RASTER_ACCEPTANCE.md): picnic view median luma 118 (path traced
with the same display 122), sunlit sand 207 (203), white suit 194 (190); the hall 56 (53 before), its floor 64 (61).
