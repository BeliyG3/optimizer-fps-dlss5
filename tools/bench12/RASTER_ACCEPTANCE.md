# pw_bench12 raster mode: measurements and acceptance

The measurement and acceptance logs of the raster mode, the baked lighting, the cascaded shadows, the ambient occlusion
and the outdoor yard. The options and how each feature works: [RASTER.md](RASTER.md); baking and the yard:
[RASTER_BAKED.md](RASTER_BAKED.md).

## Raster against path tracing (2026-10-05, RTX 4080 SUPER, the example's lab command at 3840x2160)

**Look against path tracing.** Mean linear luminance of the tonemapped dumps (sRGB decoded, Rec.709), raster divided
by path tracing with `--upscaler rr` at the same frame:

| Frame | ambient 0 | **0.165 (default)** | 0.25 |
| --- | ---: | ---: | ---: |
| snap 0 | 0.72 | **0.83** | 0.90 |
| snap 3 (mid-turn) | 0.74 | **0.85** | 0.91 |
| snap 30 (dark wall view) | 0.73 | **1.16** | 1.44 |
| combo 90 | | **0.84** | 0.89 |

No single ambient puts all four frames within ±15 %:
- the lit views lack the path tracer's reflections of the lamp tubes on the glossy floor and its bounce light, and
  ambient cannot add those;
- the dark view gains ambient and unshadowed lamp light where path tracing is nearly black.

0.165 minimises the worst deviation (−17 % on snap 0, +16 % on snap 30). Predicted from the per-pixel model: 0.2 puts
the lit snap frames at 0.86-0.87 and the dark view at 1.27; 0.1 puts the dark view at 0.97 and the lit frames at
0.78-0.80.

**Speed.** Raster alone, `--upscaler none --nr off`: GPU frame 8-10 ms, raster passes 2.8-4.6 ms. Path tracing with RR
on the same frames: 26-29 ms.

**With NR and the add-on** (release 2026.10.2, ReShade runtime as `run_nrhost`): `--render raster --upscaler none
--nr native` runs NR on the native frame. The add-on logs warped evaluates and the temporal mode, and with own
vectors its carried frames follow the snap turn. `--host core --core-flow --motion none --upscaler sr` logs
`temporal motion source: optical flow` and `hybrid field active`. The direct host fixes TemporalEvery 4,
ModelPasses 1 and SpreadPasses 0.

## Baked lighting: calibration of the scales (2026-10-06, RTX 4080 SUPER, the lab command at 3840x2160)

The values (`"scaleFull": 1.3, "scaleSun": 1.0`) and where to set them: "Scales" in [RASTER_BAKED.md](RASTER_BAKED.md).

**How they were found.** The baked raster's colour before the tonemap is affine in the two scales per pixel. Three runs
at (1, 1), (0.5, 1) and (1, 0.5) separate each pixel's rest and sun parts, after the Filmic curve is inverted; the
specular term does not depend on the scales and stays in the constant part. The model then predicts any pair; it
reproduced the measured runs exactly. Luminance is the mean linear Rec.709 luminance of the tonemapped dumps, divided
by path tracing with `--upscaler rr` at the same frame (the raster mode's method).
- **`scaleSun` 1.0:** the sun-lit back wall under the skylight (snap 0 and combo 90) gives 1.01 and 0.99, the physical
  value (the baked sun is E·cos/π in both renderers). Without specular it needed 1.7; the extra 0.7 stood in for the
  wall's sun specular. No floor inside the hall gets the sun at the standard sun direction, so the patch is a wall.
- **`scaleFull` 1.3:** among the pairs that put all four frames within ±15 %, the one with the widest margin at
  `scaleSun` 1. 1.35 would push snap 30 to 1.12.

| Baked raster / path tracing | no specular, 1, 1 | no specular, 1.3, 1.7 | specular, 1, 1 | **specular, 1.3, 1.0** | dynamic raster (ambient 0.165) |
| --- | ---: | ---: | ---: | ---: | ---: |
| snap 0 | 0.638 | 0.727 | 0.801 | **0.876** | 0.830 |
| snap 3 (mid-turn) | 0.592 | 0.668 | 0.825 | **0.885** | 0.845 |
| snap 30 (dark wall view) | 0.790 | 0.962 | 0.908 | **1.089** | 1.161 |
| combo 90 | 0.678 | 0.771 | 0.832 | **0.899** | 0.842 |
| sun wall, snap 0 / combo 90 | 0.93 / 0.91 | 1.01 / 0.99 | 1.00 / 0.99 | **1.01 / 0.99** | 0.98 (snap 0) |
| side walls, snap 0 | 0.67-0.71 | 0.86-0.91 | 1.44-1.53 | **1.68-1.78** | 3.25 (left) |
| shelves, snap 30 / combo 90 | 0.86 / 0.89 | 1.10 / 1.16 | 1.33 / 1.32 | **1.61 / 1.63** | 2.58 (snap 30) |
| foreground floor, snap 0 | 0.27 | 0.40 | 0.42 | **0.55** | 0.32 |

**The ±15 % target is met on all four frames** (0.88-1.09). It is met on the frame means, not region by region:
- **The polished floor stays dark** (0.55). The floor (roughness 0.12) reflects one highlight per lamp group at its
  centroid's mirror point; path tracing reflects every tube where it is, and the lit walls. Reflections of the
  environment need probes or SSR.
- **The lamp-lit walls and shelves are too bright** (1.6-1.8). The lamps' specular is unshadowed and comes from one
  point per group, so the side walls, seen at a grazing angle, get a broad sheen that path tracing does not have. The
  dynamic raster has the same sheen (same code) on top of its ambient.
- The two errors partly cancel in the frame means. Lowering `scaleFull` to bring the walls to 1 drops the lit frames
  below 0.75.

**Characters' sun shadows on static surfaces.** They work only where the bake has sun. At the standard sun, the
skylight's sun falls on the back wall; the only sun-lit floor is outside the hall. The characters never stand in that
light, so the bench frames show no character shadow on a static surface. Lamps give characters no shadows in raster
mode.

**Cost** (31- and 181-frame runs, measured without the specular term): GPU frame 9-11.5 ms (raster passes
2.8-4.3 ms), against 7.9-8.9 ms (2.7-3.5 ms) for the dynamic raster; loading the atlas takes 2 s; VRAM +537 MB for the
atlas. With the specular every static pixel also runs the lamp loop, as dynamic pixels do; its cost is not measured
yet (the specular runs were paged by another application's VRAM use).

## Cascaded shadows (`--csm`): measured

The option and the method: "Cascaded shadows" in [RASTER.md](RASTER.md).

**Measured (2026-10-06, RTX 4080 SUPER, the lab snap command at 3840x2160, 31 frames):**
- GPU frame 10.7 ms (raster passes 3.9 ms) with `--csm --csm-distance 40`. Without `--csm`: 11.1 ms (3.4 ms). The
  100 m runs had the debug layer, so their times are not comparable.
- **In the lab, the cascades are no sharper than the single map.** The single map, fitted to the hall, has finer
  texels at the back wall than the far cascade of a 100 m view. The skylight's sun patch on that wall comes out
  blurrier, and its window-bar shadows are lighter. A shorter `--csm-distance` sharpens the patch, but at 40 m parts
  of the hall lie beyond the last cascade and turn sunlit.
- At the standard sun, no sun reaches the floor or the characters near the camera. The near cascades change nothing
  visible there: only pixels on and around the sun patch differ.

## Ambient occlusion (`--ao clean`): measured

The option and the method: "Ambient occlusion" in [RASTER.md](RASTER.md).

**Measured (2026-10-06, RTX 4080 SUPER, the lab snap command at 3840x2160, 31 frames, 5 runs):**
- Without `--ao` the dumps 0, 3 and 30 are byte-identical to the c4f93cf build. `--ao clean` with `--debug-layer`
  (raster, baked, and baked or reverse-depth raster with `--csm`): 0 errors.
- `--depth reverse --csm --ao clean` matches the standard-depth `--ao clean` frame outside the sun patch to within
  1/255 except for about 200 isolated pixels.
- Raster passes about 5.2 ms with `--ao clean` against 3.4 ms without (the AO runs had the debug layer, so this is
  only indicative).
- **Fix round 1.**
  - Clean mode rotates the 8 directions and offsets the steps per pixel in a fixed 4x4 pattern (not time-varying),
    which the 4x4 blur integrates. This removes the streaks behind thin occluders.
  - The prepass stores the geometric normal, not the normal-mapped one, so normal-mapped surfaces no longer speckle.
  - The default radius is now 1 m, chosen by measurement. Floor luma under the left desk top relative to the open
    floor in front of it, at frame 0 of the lab snap (`<bench root>\scripts\luma_under_desk.py`; local measurement
    scripts, not in the repo):


| Run | under/open |
|---|---:|
| path traced rr | 1.260 |
| raster `--ao-radius 1` | 1.263 |
| raster `--ao-radius 0.5` | 2.219 |
| raster without AO | 2.652 |

## Game-style AO (`--ao game`): measured

The method: "Game-style AO" in [RASTER.md](RASTER.md).

- **Measured (2026-10-06, the lab command at 3840x2160, `--upscaler none`, 5 runs):**
  - `--ao clean` dumps 0, 3 and 30 (snap) are byte-identical to Task 3's fix build. Of the existing `.cso` only
    `raster_ao.cso` changed: an `acos` clamp of the horizon cosine. Without the clamp it compiles to the old bytes.
  - Raster passes: 4.1 ms with `--ao game` against 5.9 ms with `--ao clean` (static camera, 61 frames).
  - Static camera, mean |Δ| between frames 30 and 31 outside the characters, in levels of 255
    (`<bench root>\scripts\ao_noise_stats.py`; local measurement scripts, not in the repo):

| Frame | `--ao-alpha` | outside the characters | occluded pixels (AO below 200/255) |
| --- | ---: | ---: | ---: |
| AO buffer, the shifted 4x4 set and 4x4 blur this replaced | 0.1 | 0.047 | 0.36 |
| AO buffer | 0.1 | 0.20 | 0.96 |
| AO buffer before the temporal filter (= `--ao-alpha 1`) | 1 | 2.80 | 13.4 |
| final frame | 0.1 | 0.032 | |
| final frame | 1 | 0.31 (107k px above 2) | |
| final frame, `--ao clean` | | 0.011 (4.6k px above 2) | |

  - The filter is an exponential average. For per-frame noise whose frame-to-frame change is d, the filtered buffer
    changes by about α·(d/√2)·√(1 + α/(2−α)). That gives 0.20 for α = 0.1 and d = 2.8, about 0.65 for α 0.3 and
    1.1 for α 0.5.
  - In the final frame the change stays small even with no history: the AO scales only the ambient and lamp diffuse
    light, a small share of the hall's light.
  - A walking character leaves a short dark trail behind its feet in the AO buffer (the floor's history follows the
    floor, not the feet). In the final frame the trail is faint.
  - `--render-switch-at 20` from path tracing with `--ao game --motion none --debug-layer`: 0 errors, frame 21
    without garbage, no NaN.

## Outdoor yard: first look

The build and the bake: "Outdoor yard" in [RASTER_BAKED.md](RASTER_BAKED.md).

**First look** (combo frames 900, 960 and 1000 at 3840x2160, the lab command with `--exposure 0.025`):
- **Path tracing (`--upscaler rr`)** shows the yard lit by the sun, with sharp shadows of the hall, poles, crates and
  fence. GPU frame 29-30 ms.
- **The baked raster** (`--gltf lab_scene_outdoor_baked.glb --lightmap lab_outdoor_lightmap.json`) matches it closely
  by eye. It loads 9 lamp lights: the lab's 6 and the 3 poles. GPU frame 10.1 ms.
- **The dynamic raster with `--csm`** shows the same shadows. GPU frame 10.8 ms.

## CSM, AO and the yard: acceptance (2026-10-06)

RTX 4080 SUPER, 3840x2160, 20 runs. The yard frames use `--exposure 0.025` for path tracing and raster alike (the
lab's 0.22 blows the sunlit yard out; auto exposure stays off). Path tracing uses `--upscaler rr`.

**Unchanged paths.** Without the new flags the raster and baked snap dumps 0, 3 and 30 are byte-identical to the
c4f93cf build, and path tracing (rr, combo 450-452) to the ea809e47 baseline. All 12 default `.cso` are
byte-identical to master's, the 3 baked ones to c4f93cf's.

**Cascades in the yard** (`--csm`, defaults: stable, 100 m):
- **Shadows:** the hall's, the poles', the crates', the buildings' and the fence's sun shadows match path tracing in
  shape and position, from 2 m to about 30 m (combo 900, 990 and 1080).
- **Seams:** none visible where shadow edges cross the blend bands of cascades 0→1 (6.0-6.7 m), 1→2 (14.1-14.9 m)
  and 2→3 (30.4-32.1 m). The band positions were read from a scratch shader that writes the cascade index.
- **Stability.** The camera is a snap turn of −90° in the yard, at combo frame 1080's pose.
  - The cascades are a pure function of the camera pose. So in the held frames after the turn (f6 = f30 = f31) the
    frames are byte-identical in both modes, and frame-to-frame shimmer there is 0 for `--csm-stable 0` as well.
  - The difference shows while the view turns. Each 15° step is warped back by the rotation homography, and the
    sun visibility is compared on the shadow edges (visibility 0.1-0.9, read from the scratch shader), within one
    cascade.
  - `--csm-stable 1` changes by 0.023-0.049 (mean 0.031); 5-16 % of the pixels change by more than 0.1. This is the
    resampling floor of a 3-4 px penumbra: the map itself is locked to the world.
  - `--csm-stable 0` changes by 0.050-0.120 (mean 0.076, 2.4 times as much); 10-50 % of the pixels change by more
    than 0.1.
  - In the final frame's luma on the shadow edges, the stable mode changes by 7-13 levels and the unstable mode by
    12-21. On geometric edges both change by 19-21 levels.
- **GPU frame:** 10.2 ms with `--csm`, 20.1 ms with `--csm --ao clean` (yard combo). Another application loaded the
  GPU during these runs, so the times are indicative only.

**AO:**
- **Lab, clean:** floor luma under the left desk relative to the open floor (snap f0): path tracing 1.260,
  raster `--csm --ao clean` 1.263 (no AO 2.652).
  - Baked `--csm --ao clean` gives 0.409 (no AO 1.105). The AO scales the bake's `rest`, which already holds the
    desk's occlusion, so the floor under the desk is darkened twice.
  - Fixed after the acceptance (ruling): in baked mode SSAO no longer touches static surfaces with baked light, whose
    occlusion is in the bake; characters and static triangles without lightmap UVs keep it (RasterShade). This
    supersedes the spec's "AO on the baked `rest`".
- **Yard, clean:** no visible effect. The AO scales the ambient and lamp diffuse light. At exposure 0.025 that light
  is close to black in raster (shaded walls read black, path tracing grey), so `--ao clean` changes under 20 pixels by
  more than 2 levels per frame. The bake's `rest` outdoors is just as dark. The yard's corners do not show the path
  tracer's sky occlusion.
- **Game, held camera:** baked lab with `--ao game --ao-alpha 1` (no history). The final frame changes by 0.29
  levels (mean) from frame 30 to 31 outside the characters, and 52k pixels change by more than 2 levels. The trail
  needs the history (α 0.1): see "Game-style AO" in [RASTER.md](RASTER.md).
- **Lab luminance / path tracing** (snap 0 / 3 / 30, combo 90):
  - raster `--csm --ao clean`: 0.835 / 0.857 / 1.104 / 0.853. Snap 0 is 1.5 points outside ±15 %. Raster without
    flags already gives 0.830 / 0.845 / 1.161.
  - baked `--csm --ao clean`: 0.871 / 0.884 / 1.057 / 0.896. Without flags: 0.876 / 0.885 / 1.089 / 0.899.
- **Yard luminance / path tracing** (combo 900 / 930 / 960 / 990 / 1020 / 1050 / 1080):
  - raster `--csm`: 0.98 / 0.98 / 0.97 / 0.97 / 0.96 / 0.92 / 0.87. With `--ao clean` the values are the same.
  - baked `--csm --ao clean`: 1.01 / 1.00 / 1.00 / 1.00 / 0.98 / 0.94 / 0.89.
  - The last frames look at the fence's shaded side, which raster leaves black.

**Add-on** (ReShade 6.8 + the 2026.10.2 add-on/core, `--nr native --hud`, TemporalMode 1, N = 3, against
TemporalMode 0):

| Run | Evaluations | Full | Carried | Fallback | PSNR vs T0 |
| --- | ---: | ---: | ---: | ---: | --- |
| raster lab `--csm --csm-stable 0 --ao game`, snap, 300 frames | 299 | 100 | 199 | **0** | 31.8-38.1 dB in the turn (f120-126), 39.0 dB held (f150-152) |
| baked yard `--csm --ao game`, combo, 1081 frames | 1080 | 360 | 720 | **0** | 39.3-39.9 dB (f990-993), 34.1-34.5 dB (f1074-1080, fence) |

The largest differences against T0 are on thin structures: the fence, the skylight bars and the walking girl's floor
reflection during the turn.

**Debug layer:** baked lab with `--csm --csm-stable 0 --csm-distance 100 --ao game --ao-alpha 1 --ao-radius 1
--ao-strength 1 --debug-layer` gave 0 errors and 0 corruption. There were 10 warnings: the known "Ignoring
InitialState" (8× COPY_DEST, 2× UNORDERED_ACCESS).

## Garden, wind, HDRI sky, probes, lamp tubes, lamp shadows and FXAA: acceptance (2026-10-07)

RTX 4080 SUPER, 3840x2160, 24 runs, PNG dumps. The garden is `lab_scene_garden_baked.glb` with
`lab_garden_lightmap.json` (rest, sun and lamp maps), `--hdri autumn_field_puresky_nosun.hdr` and `--exposure 0.007`;
the yard uses 0.025 and the lab 0.22. Path tracing uses `--upscaler rr`. "All flags" in raster means `--csm --ao clean
--probes --lamp-tubes --lamp-shadows --raster-sky-ambient --fxaa` (the lab has no HDRI, so no sky ambient there).

**Unchanged paths.** Without the new flags the decoded pixels are identical to the earlier references on all 25
frames: lab raster, baked, raster `--csm --ao clean` and path tracing (snap 0, 3 and 30), the yard's raster `--csm` and
path tracing (combo 900-1080) and the baked yard (900, 960, 1000). The lab with `--wind` writes the same PNG bytes as
without it and prints one warning (no `veg_` nodes). All 159 `.cso` built before FXAA are byte-identical; FXAA added one.

**Look** (garden, combo 900 / 990 / 1080, mean linear luminance divided by path tracing):

| | 900 | 990 | 1080 |
| --- | ---: | ---: | ---: |
| baked raster, all flags | 1.04 | 1.04 | 0.97 |
| raster without `--lightmap`, all flags | 0.98 | 1.00 | 0.93 |

- The path is clear of grass (closest clump centre 0.30 m from its edge, none reaches over it).
- Trees, grass and ferns cast cascaded shadows; shaded sides are sky-lit; the sky shows the HDRI without a sun disc
  (in-paint radius 4.41°, residual peak 1.89 times the surrounding sky, limit 3).
- `--raster-sky-ambient` with `--lightmap` changes only unbaked pixels: with `--bloom 0` the grass and trees change and
  the soil, path, walls, fence and sky stay bit-identical. With bloom on, the bloom of the brighter grass spills onto
  the soil next to it.

**Wind.** Held camera on the path, `--wind --anim-playing 0` (the foliage still moves with the animation stopped).
Frame t−1 warped to frame t with the bench's motion, against frame t−1 as it is, on the 2.4 M foliage pixels in motion
(mean 1.4 px per frame):

| | frame 30 | frame 60 |
| --- | ---: | ---: |
| mean abs error, bench motion (of 255) | 15.1 | 15.8 |
| mean abs error, zero motion | 29.5 | 31.0 |
| mean abs error, motion with the opposite sign | 33.8 | 34.7 |

The remaining error is the disocclusion of thin cards. Trunks have zero motion. Cost on the combo with the characters:
CPU animation 9.3 ms per frame against 5.15 ms without wind (4 trees, 4872 grass clumps, 15 ferns; 1.12 M dynamic
triangles); the GPU frame grows by about 7 ms at the held view, outside the raster passes (the dynamic geometry upload).

**Grass at the alpha cut-out with `--ao clean`.** Exactly black pixels that are not black without AO: 0 at combo 990
and 1080, 16611 (0.2 %) at 900. Those lie inside dense clumps in the hall's shadow, where the grass is dim without AO
too (median 14 of 255): SSAO occludes their only light, the ambient. They are not holes: the sunlit frames have none,
and the prepass, the scene pass and the shadow passes share one alpha test.

**Lamp shadows in the lab** (all flags). The dummy's and the desks' shadows under the tubes show in raster and baked
mode; raster's are harder and darker and the baked ones fainter than path tracing's (one map per lamp group); no acne
on the floor. Luminance / path tracing, snap 0 / 3 / 30 and combo 90:
- baked (lamp-split bake): 1.056 / 0.898 / 1.132 / 0.928, within ±15 %;
- raster: 0.987 / 0.852 / 0.920.

**Add-on** (ReShade 6.8 + 2026.10.2, `--nr native --hud`, TemporalMode 1, N = 3, against TemporalMode 0) on the baked
garden with `--wind --csm --ao game --fxaa --lamp-shadows --raster-sky-ambient`, combo, 1081 frames: 1080 evaluations,
360 full, 720 carried, **0 fallback**, no submission drops. Warp acceptance 79 % (the yard: 95 %). PSNR against T0 on
the foliage: 31-33 dB at 900-903, 27-28 dB at 990-993 (trees and grass close by), 22.8 dB at 1077-1080. Full and
carried frames differ from T0 alike, so on waving grass the add-on's history drifts from T0's; the yard without foliage
gave 34-40 dB.

**Debug layer:** the baked garden with all flags, `--ao game` and `--wind` gave 0 errors and 0 corruption; 13 warnings,
all "Ignoring InitialState".

## Outdoor colours: the firefly clamp under auto exposure, and the UE5-like display (2026-10-07)

The owner's interactive garden (raster, `--csm --ao clean --probes --lamp-tubes --lamp-shadows --raster-sky-ambient --hdri
autumn_field_puresky_nosun.hdr --wind --fxaa`, sun 1500, haze 0.008, ACES, bloom 0.06, auto exposure, `--exposure 1.45`)
read washed out outdoors: flat sand, white suits a flat grey, the pink mannequins and red barrels "burnt"; the hall was
fine. 1920x1080, frame 180, 4 views (picnic table, a white suit close up, the dancers, the hall), PNG dumps; pre-display
luminance recovered by inverting sRGB and the ACES fit (`<bench root>\scripts\measure_5c.py`); outputs
`<bench root>\out\garden_5c`, sheets `<bench root>\look\24_colour_*`.

**Root cause: the firefly clamp.** `--firefly 50` limits the luminance of the colour the renderers write, which is the
radiance times `--exposure` (1.45). The auto exposure's scale is applied only later, at presentation. With the garden's sun
every sunlit surface is far over 50 (sunlit sand 125-190, the white suit up to 500), so all of them were scaled down to one
luminance, hue kept. The meter's 90th percentile then sat on the clamp (picnic: highlight target 0.20 / 55 = 0.0036, the
settled scale). Path tracing had the same clamp (picnic, PT with auto exposure: sunlit sand 99.6 % at the clamp).

| picnic view | share of the frame at the clamp | sunlit sand: at clamp / luma RMS | white suit luma p50 / p95 | settled scale |
| --- | --- | --- | --- | --- |
| before | 28.3 % | 99.5 % / 2.0 | 145 / 145 | 0.0036 |
| `--firefly 1e6` (test) | 5.4 % (natural) | 2.5 % / 23.6 | 141 / 209 | 0.00225 |
| fixed build (limit / scale) | identical to `--firefly 1e6`, byte for byte | | | 0.00225 |

The other hypotheses, one variable at a time on the owner's command (picnic; suit and dancers agree):

| hypothesis | test | result |
| --- | --- | --- |
| tone map | `--tonemap neutral` / `none` with the clamp | flatter still (luma RMS 30 / 33 vs 47); sand RMS 1.2 / 1.4: the flat sand is upstream of the curve |
| per-channel ACES burns colours | the unclamped frame through the ACES fit vs UE filmic, AgX, PBR Neutral offline (`tonemap_eval_5c.py`) | mean hue shift 1.5-2.5 degrees (AgX 1-1.9), saturation kept (0.95-1.2 of the input's); not the cause |
| exposure / metering | targets printed per dump | the highlight cap decides outdoors and in the hall; with the clamp it metered the clamp itself |
| lighting vs path tracing | fixed exposure 0.0033 (no clamp), raster vs PT RR | region luminance raster / PT 0.87-1.13 (sky 0.63), same chromaticity: lighting is not flat |
| haze | `--haze 0` | the sky also reaches the clamp (88 %), surfaces unchanged (sand RMS 1.8) |
| colour space | code: colour textures `R8G8B8A8_UNORM_SRGB`, data maps `UNORM`, one sRGB encode into a `UNORM` swap chain; same views in both renderers | no double decode/encode |
| bloom | `--bloom 0` | blacks 2 levels lower (p5 7.8 -> 5.8), sand RMS 2.1: a light veil, not the cause |
| FXAA | without `--fxaa` | identical within 1 level |

**Fix** (commit `bench12: firefly clamp follows auto exposure`): under auto exposure the limit is `--firefly` divided by the
last frame's adapted scale (`FireflyLimit`, `display.h`), so it is 50 on screen; nothing is clamped before the first
reading; without auto exposure nothing changes. Dumps: auto exposure off byte-identical to the build before; picnic with
the fix byte-identical to the old build with `--firefly 1e6`; the hall's metrics unchanged (median 53.1, floor 60.9).

**UE5-like display** (owner's addendum, DISPLAY_UE.md): `--tonemap ue-filmic --local-exposure 1 --exposure-metering ue`.
Display luma p50 / p95 per region, raster and path tracing (RR) with the same display:

| view, region | before | clamp fixed (ACES, bench metering) | UE display, raster | UE display, path traced |
| --- | --- | --- | --- | --- |
| picnic, frame | 87 / 145 | 61 / 176 | 118 / 212 | 122 / 206 |
| picnic, sunlit sand | 145 / 145 | 172 / 204 | 207 / 226 | 203 / 219 |
| picnic, white suit | 145 / 145 | 141 / 209 | 194 / 232 | 190 / 232 |
| suit view, pink mannequin | 125 / 136 | 104 / 216 | 138 / 229 | 145 / 229 |
| suit view, red barrel | 46 / 93 | 36 / 75 | 45 / 107 | 59 / 107 |
| dancers, concrete | 85 / 97 | 64 / 74 | 97 / 111 | 98 / 108 |
| hall, frame | 53 / 200 | 53 / 200 | 56 / 197 | 57 / 207 |
| hall, floor | 61 / 178 | 61 / 178 | 64 / 178 | 61 / 196 |

UE metering's settled scale: picnic 0.0076, suit 0.0052, dancers 0.0059 (1.1-1.8 stops over the bench metering); the hall
is held by `--exposure-min-ev 0.5` at 0.406 (bench: 0.340). Local exposure alone (same UE metering): picnic sand p50 219 ->
207, frame p5 12 -> 17. Debug layer (picnic, all UE options): 0 errors, 23 warnings, all "Ignoring InitialState". Time: the
display stage 1.08-1.11 ms without, 1.18-1.25 ms with the UE options (interleaved, on a GPU shared with another
application at 85 % load), frame time within its noise.

**Sky and daylight (owner, flying the UE display: "the scene under the sky should be brighter").** At fixed exposure the
raster sky across and towards the sun was 0.57-0.73 of the trace's (picnic; 1.03-1.08 looking away): the raster fog
attenuated the sun over the full 300 units for every scattering point, the trace from each point to the boundary. Now
the fog takes the trace's mean over the ray (`HazeSunTransmittance`, 16 steps, within 2.4 % of the trace's estimator):
sky / PT 0.98-1.00 (picnic three boxes, path), 1.02 (dancers). The sky SH ambient is not low: shaded ground with probes
1.09 of PT, without probes (sky SH alone, unoccluded) 1.91. Shaded vertical and glossy surfaces stay dark (path view: bench
in shade 0.47, building wall in shade 0.55 of PT; without AO 0.79 / 0.64): the AO (strength 1.5) and the missing sky
reflection (raster has no specular from the sky; the trace's varnished bench top reflects it), not the SH.
Then `--exposure-curve 3,6,0.33` (DISPLAY_UE.md): sunlit exteriors +1/3 stop, the hall none. Display luma p50/p95 with
the UE display, before (rt_fly_garden6) / after (raster) / after (path traced, same display):

| view, region | before | after, raster | after, path traced |
| --- | --- | --- | --- |
| picnic, frame | 118/212 | 137/215 | 136/214 |
| picnic, sky | 123/140 | 157/175 | 155/174 |
| picnic, sunlit sand / white suit | 207/226, 194/232 | 210/228, 198/234 | 211/225, 200/236 |
| dancers, frame / white suit / pink mannequin | 108/196, 190/230, 164/231 | 121/205, 199/235, 177/235 | 123/201, 199/235, 176/234 |
| path in sun, frame / sky / sunlit gravel | 126/230, 159/188, 228/240 | 144/231, 191/214, 230/242 | 139/230, 187/210, 228/239 |
| path, sand in shade / bench in shade | 79/128, 30/67 | 83/133, 32/71 | 73/114, 53/98 |
| under the tree, frame / tree shadow | 130/208, 194/211 | 144/215, 203/218 | 141/211, 202/214 |
| hall, frame / floor | 56/197, 64/178 | 56/197, 64/178 | 57/207, 61/196 |

The hall: raster max difference 22 levels on 0.0006 % of the pixels (the sun shaft's fog), path traced byte-identical.

## Frame carry

The add-on's frame carry on foliage (phases 1-2, 2026-10-08/09) and under camera motion (phase 3) is measured with the
scripts in [foliage/](foliage/) and [motion/](motion/README.md); the measurement logs themselves are not published.
