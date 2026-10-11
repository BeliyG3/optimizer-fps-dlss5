# pw_bench12 raster mode (`--render raster`), `--motion none`, `--camera snap`, `--hud`

A game-like frame of the same scene without ray tracing: conventional depth, a shadow-mapped sun, lamps as point
lights, no indirect bounce, analytic fog, no jitter without an upscaler. Everything after the render (motion pack,
DLSS SR, NR, the add-on, presentation, dumps, timing) is the path tracer's. `--render pathtrace` (the default) is
unchanged.

This file is the reference: the options and how each feature works. Related files:
- [RASTER_BAKED.md](RASTER_BAKED.md): the baking workflow, rendering with the lightmap (`--lightmap`) and the outdoor yard.
- [RASTER_ACCEPTANCE.md](RASTER_ACCEPTANCE.md): the measurements and acceptance logs of all of the above.
- [DISPLAY_UE.md](DISPLAY_UE.md): the display as UE5's (`--tonemap ue-filmic`, `--local-exposure`, `--exposure-metering ue`); [RASTER_REFLECTIONS.md](RASTER_REFLECTIONS.md): the sky's reflections (`--sky-reflections`) and why `--ao-strength` is 1; [foliage/](foliage/): the scripts that measure the add-on's frame carry on foliage (`--view vegmask`, `--motion-span`).

## The frame

- **Passes** (`raster*.cpp`, `shaders/raster*.hlsl`): a 2048² depth-only sun shadow map, orthographic and fitted to
  the scene bounds; one scene draw with the path tracer's materials (textures, normal maps, two-sided, alpha cutoff
  clipped in both passes); a compute resolve that writes the same `colour`/`depth`/`motion` guides the trace writes
  and clears the RR guides.
- **Lighting:** emission; the sun with 3x3 PCF shadows; one unshadowed point light per lamp group at its
  power-weighted centroid (shadowed with `--lamp-shadows`), intensity `power / 4` (closed Lambertian lamps), following the menu's group intensity and
  tint; `--raster-ambient` times the albedo for the missing bounce light.
- **Sky and fog:** the sky where nothing was drawn. Fog is analytic, per pixel, as games draw distance fog:
  `colour * T + 0.9 * (1 - T) * (sun * visibility + ambient) * exposure`, with `T = exp(-haze * distance)`.
  - Sun is `--sun-strength` times the Henyey-Greenstein phase (`--haze-g`) of the angle between the view ray and the
    sun, attenuated like the trace's sun from each scattering point to the medium's boundary (see below).
  - `visibility` is the shaded surface's sun shadow (the pixel's one shadow-map lookup, 1 for the sky), so shadowed
    surfaces get no sun haze. This is the usual cheap game approximation.
  - Ambient is the constant grey `--raster-ambient`.

  There are no lamps in the fog and no sampling along the ray, so there are no sun shafts and no noise. The firefly
  clamp is the trace's: `--firefly` on screen, so under `--auto-exposure 1` divided by the adapted scale (`FireflyLimit`).
  Until 2026-10-07 it was not (every sunlit garden surface clamped to one luminance), and the sun's attenuation was a constant
  `MediumTransmittance(--haze, 300)` (`HazeSunTransmittance` now: the sky towards the sun was 0.57-0.73 of the trace's); RASTER_ACCEPTANCE.md, "Outdoor colours".
- **Depth and motion:** the `--depth` conventions of the trace (hardware depth is converted for the linear modes);
  motion current-to-previous from the unjittered matrices, `--motion ndc` included.
- **Jitter:** only with `--upscaler sr`; `--upscaler rr` is rejected (Ray Reconstruction needs path-traced input).
- **Acceleration structures** are not touched on raster frames. The animated vertices are still uploaded. If the
  animation moved, the next path-traced frame rebuilds the dynamic BLAS and the TLAS first. The render mode comes from
  the command line or `settings.json` (the menu has no render selector); within a run, only `--render-switch-at`
  changes it.
- The `path trace` figure of the GPU time line is the raster passes in this mode. Interactive triangle picking
  reports nothing in raster mode.

## Options

| Option | Default | Meaning |
| --- | --- | --- |
| `--render pathtrace\|raster` | `pathtrace` | Path tracing, or the raster frame above. |
| `--render-switch-at F` | none | Scripted runs: frame F flips the render mode (raster to path tracing or back), as a settings change between frames would. F below the frame count; not with `--upscaler rr` or `--interactive`. Checks the acceleration-structure rebuild after raster frames. |
| `--raster-ambient A` | `0.165` | Ambient radiance per unit albedo on surfaces, and the fog's ambient grey, before exposure. Tuned against path tracing (see [RASTER_ACCEPTANCE.md](RASTER_ACCEPTANCE.md)). |
| `--raster-sky-ambient` | off | The ambient term is the `--hdri` sky's irradiance by the surface normal (nine SH coefficients), not the constant. Requires `--render raster` and `--hdri`; see "Sky ambient". |
| `--probes` | off | The ambient term comes from an irradiance probe grid traced once at start; see "Light probes". Requires `--render raster`. `--probe-spacing 1` (m, 0.1..100), `--probe-rays 256` (64..16384) and `--probe-dump file` (the grid as floats, layout in `probes.cpp` `WriteDump`) require it. |
| `--lamp-tubes` | off | Each emissive tube of a lamp group is a line light (one-sided for ceiling fixtures, range-culled); screens and other compact emitters stay point lights. Requires `--render raster`; see "Lamp tubes". |
| `--lamp-shadows` | off | A spot shadow map per lamp group shadows its point light and tubes, as a stationary light in Unreal. Requires `--render raster`; with `--lightmap` a bake with the `"lamp"` map. See "Lamp shadows". |
| `--fxaa` | off | FXAA 3.11 on the finished raster frame, in the resolve, before the upscaler, NR and the add-on. Requires `--render raster`; see "FXAA". |
| `--wind` | off | Both render modes: `veg_*` mesh nodes sway in the wind; see "Wind". `--wind-strength S` (0..100, default 1) scales the displacement and requires `--wind`. With no `veg_` node in the scene: one warning, nothing changes. |
| `--lightmap file.json` | none | Static geometry lit from baked lightmaps (see [RASTER_BAKED.md](RASTER_BAKED.md)). Requires `--render raster` and a glb with lightmap UVs (`lab_scene_baked.glb`). |
| `--auto-exposure 1` | off | Both render modes: eye adaptation in stops, `--exposure-speed-up 3` towards a brighter scene (out of the hall: 90 % in about 0.8 s) and `--exposure-speed-down 1.5` towards a darker one (back in: about 1.5 s), real frame time when interactive; metered on the average with highlight protection (the 90th percentile at most 0.20 before the tone curve), `--exposure-ev` and `--exposure-centre` on top; the firefly clamp follows the scale; see README. |
| `--motion none` | | Both render modes: the motion guide is zero after the render, as in a game without vectors. Under the ReShade add-on (`--host ngx`) the add-on then carries along zero vectors. The core's optical flow becomes the motion source only with the bench's direct core host, `--host core --core-flow` (which needs `--upscaler sr --nr-colour srgb`); `--core-flow` without `--host core` is rejected. |
| `--camera snap` | | An in-place turn of `--snap-deg 90` over `--snap-frames 6`, held `--snap-hold 54` frames, then back (frames are 1/60 s). |
| `--hud` | | Both render modes: a game-like HUD blended into the render colour after the render, before DLSS and NR. A bar top-left, three text labels top-right (the third blinks, 30 frames on and 30 off), a minimap ring with three diamonds bottom-right. Laid out for 1080p and scaled with the render height. White is 4.0, the panels 0.02 (scene-linear, after exposure). Not in `--view accum`; in the accumulated copy NGX gets with the menu's "feed accumulated image". |

```bat
pw_bench12.exe 120 --gltf ..\bench\assets\lab_scene.glb --render raster --upscaler none --render-scale 1 --camera snap --sun-dir 0.45,-0.77,0.45 --sun-strength 1500 --exposure 0.22 --haze 0.008 --fov 62 --width 3840 --height 2160 --dump 0,3,30
```

Dumps are 4K BMPs by default (33 MB each). `--dump-format png` writes `dump_N.png` instead: the same pixels, lossless, about 3-5x smaller (recommended); BMP stays the default so reference hashes keep matching.


## Cascaded shadows (`--csm`)

`--csm` replaces the single sun shadow map with four cascades fitted to the view each frame, in raster and baked
mode. Without it nothing is created (no array, descriptor, root signature or PSO) and the frame is unchanged.

| Option | Default | Meaning |
| --- | --- | --- |
| `--csm` | off | Four cascades of 2048² (one `R32` array, a D32 view per slice). Requires `--render raster`. |
| `--csm-stable 0\|1` | `1` | 1: each cascade is sized by its slice's bounding sphere (plus a one-texel border) and its origin snaps to its texel grid, so the shadows do not swim when the camera moves. 0: fitted tightly to the slice each frame. |
| `--csm-distance D` | `100` | View depth (m) of the last cascade's far end. Beyond it the sun is unshadowed, as outside the single map. |

- **Slices** (`raster_csm.h`): practical splits, λ 0.75, from the camera's near plane (0.1) to `--csm-distance`. Each
  cascade is an orthographic map along the sun's travel. Its depth covers the scene bounds, so casters outside the
  slice still cast. Slice c+1 is fitted from the start of slice c's blend band.
- **Lookup** (`shaders/raster_csm.hlsli`): the cascade is chosen by the pixel's view depth (along the unjittered
  camera's forward). In the last 10 % of a slice the result blends linearly into the next cascade. Each lookup is the
  single map's 3x3 PCF. The depth bias is one texel of each cascade (its `texelWorld / depthRange`, computed per
  cascade on the CPU), as the single map's is one of its texels. The normal offset is the single map's.
- **Fog** reads the pixel's cascaded sun visibility, as it read the single map's.
- **Baked mode:** the dynamic-only map becomes a second 4-slice array, drawn with the same cascades from the dynamic
  triangles alone. The full shadow (dynamic pixels, the sun specular, the fog) uses the first array.
- **Shaders:** `raster_csm_ps.cso`, `raster_csm_lightmap_ps.cso`, `raster_csm_shadow_vs.cso` and
  `raster_csm_shadow_dynamic_vs.cso`, built from the raster sources with `PW_CSM=1`. The scene pass keeps
  `raster_vs.cso` (`raster_lightmap_vs.cso` in baked mode). The CSM root is graphicsRoot's parameters (the baked
  root's in baked mode), then the cascade constants (`b2 space1`), the array (`t3 space2`) and the shadow pass's
  cascade index (`b3 space1`).

## Ambient occlusion (`--ao clean|game`)

`--ao clean` and `--ao game` add screen-space ambient occlusion in raster and baked mode, alone or with `--csm`.
Without `--ao` nothing is created (no target, descriptor, root signature or PSO), the default, baked and CSM shaders
are byte-identical and the frame is unchanged.

| Option | Default | Meaning |
| --- | --- | --- |
| `--ao clean` | off | A depth/normal prepass, then GTAO and a depth-aware blur before the scene pass. Requires `--render raster` (or `--render-switch-at` into it). |
| `--ao game` | off | As a game does it: 2 directions with per-frame noise, a 2x2 blur, then a temporal filter (below). |
| `--ao-alpha A` | `0.1` | `--ao game` only, 0 < A <= 1: the temporal filter's weight of the current frame. `1` keeps no history, so the per-frame noise reaches the frame. |
| `--ao-radius R` | `1` | World radius (m) of the horizon search, projected to pixels and clamped to an eighth of the render height (at least 64 px). |
| `--ao-strength S` | `1` | The visibility is raised to this power (1.5 until 2026-10-07: RASTER_REFLECTIONS.md). |

Where it applies: raster, all the indirect light (the ambient term: the constant, the sky's SH9 or the probes); baked, only
pixels shaded without baked light (characters, static triangles without lightmap UVs). Baked static surfaces keep the
bake's own occlusion. Direct light (sun, lamp points and tubes) has its shadows and is never occluded.

- **Prepass** (`shaders/raster_prepass.hlsl`): the scene pass's own vertex shader (`raster_vs.cso`, or
  `raster_lightmap_vs.cso` in baked mode) draws the depth buffer and the octahedral geometric normal (RG16_SNORM,
  the triangle's plane turned toward the camera), with the alpha cutoff. The scene pass then tests depth EQUAL and
  does not write it; the same vertex shader makes the depths bit-identical.
- **`CSAo`** (`shaders/raster_ao.hlsl`): one thread per pixel. 8 slice directions, rotated per pixel by a fixed 4x4
  interleaved pattern (`AoPattern`) that also shifts the 6 steps each way (spaced quadratically), over `--ao-radius`
  projected to pixels (at most `AoMaxPixels`, an eighth of the render height: the old fixed 64 px cut 1 m to 14 cm at 4 m
  in 4K, so props left almost no contact darkening). The view position comes from the depth (both depth conventions) and the jittered pixel ray.
  Each slice takes its two horizons and integrates GTAO's cosine-weighted visibility with the slice's projected
  normal. Samples fade toward the tangent plane between 60 % and 100 % of the radius. The mean over the slices is
  raised to `--ao-strength`.
- **`CSAoBlur`:** a 4x4 box weighted by `exp(-|Δz| / (0.05 z))`, into the R8 texture the scene pass reads at `t4 space2`.
- **What it darkens:** raster mode, the indirect light as games apply AO: the ambient term, whatever feeds it (the
  constant, the sky's SH9, the probes with the sun's and lamps' bounce light). Until 2026-10-07 it also darkened the lamps'
  direct diffuse and not more than the ambient. Baked mode: only the pixels shaded without baked light (characters, static triangles without lightmap UVs); baked
  static surfaces keep the bake's occlusion. Never the sun (direct or baked) and never any specular.
- **Shaders:** `raster_prepass_ps.cso`, `raster_prepass_lightmap_ps.cso`, `raster_ao.cso`, `raster_ao_blur.cso`, and
  the scene pixel shaders `raster_ao_ps.cso`, `raster_ao_csm_ps.cso`, `raster_ao_lightmap_ps.cso`,
  `raster_ao_csm_lightmap_ps.cso` (`PW_AO=1` with or without `PW_CSM`, `PW_LIGHTMAP`). The AO scene root is the run's
  scene root (graphicsRoot's, the baked or the CSM root's parameters) plus the AO table; the prepass uses it too.
- **Resize** (and `--render-switch-at` into raster): the AO targets are dropped with the raster targets and recreated
  at the next raster frame; with `--ao game` the history is then not used for that frame (below).

### Game-style AO (`--ao game`)

- **Prepass:** `PrepassMotion` (`raster_prepass_motion_ps.cso`, `raster_prepass_motion_lightmap_ps.cso`) also writes
  the motion (RG16F): the scene pass's formula (unjittered matrices, current-to-previous), always in pixels. The AO's
  temporal pass runs before the scene pass, so it cannot use the scene pass's motion target. The scene pass still
  writes that one unchanged. With `--motion none` only the scene's output motion is zeroed: the AO filter is the
  game's own and keeps the true vectors.
- **`CSAo`** (`raster_ao_game.cso`, `PW_AO_GAME=1`): 2 slice directions. The rotation is interleaved gradient noise
  (Jimenez) at the pixel moved by 5.588238 px per frame (`AoGameNoise`, 64 frames). The step offset is the same noise
  at the transposed pixel. Unlike a shifted 4x4 set, no window holds the same values on the next frame.
- **`CSAoBlur`** (`raster_ao_blur_game.cso`): a 2x2 depth-aware box, so the per-frame noise is only partly averaged,
  as in a game that relies on TAA.
- **`CSAoTemporal`** (`shaders/raster_ao_temporal.hlsl`): each pixel follows the prepass motion into the previous
  history (bilinear) and writes `lerp(history, current, w)`. `w` is `AoHistoryWeight` (`ao_math.h`): 1 when the
  history is not valid or the sample is rejected, else `--ao-alpha` (0.1). Rejected: off screen, or the stored view depth differs
  from this pixel's by more than 10 %.
  - The history ping-pongs between two R16G16_FLOAT textures (the AO, the view depth for the next frame's test). The
    scene pass reads this frame's.
  - The history is valid only on the frame right after the last AO frame on the same targets (`AoHistoryUsable`).
    Create, resize, `--render-switch-at` into raster and any path-traced frame in between all invalidate it.

## Sky ambient (`--raster-sky-ambient`)

With an HDRI the sky is the light of the shade, but the constant `--raster-ambient` knows nothing of it: at exposure 0.025
the yard's shaded sides read black while path tracing shows them blue-grey.
- **Projection (CPU, once at start).** `SkyIrradianceSH` (`sky_sh.h`) projects the whole panorama on 9 SH coefficients, the
  lower hemisphere too, exactly as the path tracer's rays see it. A texel's direction and solid angle follow `Sky()` in
  `shaders/scene.hlsli` (`u = atan2(z,x)/2pi + 0.5`, `v = acos(y)/pi`, row 0 up, texel centres). Then each band is
  convolved with the cosine lobe (pi, 2pi/3, pi/4): `EvalSH(c, n)` is the irradiance at normal n (pi * L for a constant sky).
- **Shading (`PW_SKYAMB`).** In `RasterShade` the ambient term becomes `albedo * max(SH(n), 0) / pi`, in place of
  `rasterAmbient * albedo`, and `--ao` scales it as before. Nothing else changes: the fog's ambient grey stays
  `--raster-ambient`, and with `--lightmap` baked static pixels keep their baked light (the sky is in the bake); the SH
  applies only where `RasterShade` runs (dynamic triangles, foliage, static triangles without lightmap UVs).
- **Binding.** The 9 float4 are a constant buffer at `b4 space1` (free in every scene root: the raster root has b0 and
  b1, the baked one b2, the CSM one b2 and b3 in space1), written once. It is the last parameter of the scene root, after
  the baked and cascade parameters and before `--ao`'s table; `Raster::skyParameter` is its index. Each pixel-shader
  variant has a `raster_sky_*_ps.cso` twin (`build.cmd`, 8 of them: plain, `lightmap`, `csm`, `csm_lightmap`, and each with
  `ao`); the 32 earlier `.cso` and every run without the flag are byte-identical.
- **Yard, combo frame 990, `--csm`, exposure 0.025, 3840x2160** (`look\18_skyamb_*`): the shaded side of the right-hand
  building, luma 1.0 without the flag, 50.8 with it, 121.7 path traced (`--upscaler rr`, the same `--hdri`). The sunlit
  ground changes from 183.8 to 187.8 (path tracing 192.0). Raster still has no bounce light, so the shade is about 40 % of
  the path-traced value: the sunlit ground and walls light it in the trace.

## Light probes (`--probes`)

A global ambient (the constant or the sky's SH) lights a character inside the hall with the same light as in the yard: with
`--raster-sky-ambient` the whole hall interior turns sky-blue. `--probes` gives the ambient a place: a grid of irradiance
probes, as Unity's light probes or Unreal's volumetric lightmap.
- **Grid** (`probes.h`, `ProbeGridLayout`): centred on the scene's bounds, `ceil(extent / spacing)` probes per axis, each
  standing for a cell of one spacing, so the cells cover the bounds. Above 256 k probes (or 2048 on an axis) the spacing
  grows in 2 % steps. Lab: 20x12x37 = 8880 probes at 1 m; yard: 60x17x60 = 61200.
- **Trace, once at start** (`probes.cpp`, `shaders/probes_trace.hlsl`): one group of 64 threads per probe, inline
  `RayQuery` as the path tracer, its own `Trace`, `GetSurface`, `Direct` (sun with its shadow ray, lamps by RIS), `Sky`
  and BSDF sampling. Rays: a fixed spherical Fibonacci set (`--probe-rays`). Each ray takes the sky (or `--hdri`) on a miss;
  on a hit the light the surface reflects (its direct sun and lamps, then one BSDF-sampled bounce with the next surface's
  direct light and its MIS-weighted emission, as `Radiance()`). Emission seen directly counts only for emitters without a
  lamp group: lamp groups are raster's own point lights, so counting them again would double them. The lamps' indirect
  light is in: at each hit `Direct` samples the emissive lamp triangles (RIS, a shadow ray) through the surface's BSDF,
  at the first and the bounce hit, as the path tracer does. Lab, all lamps at intensity 0 for a check: the median probe
  at 0.3 m loses 82 % of its ambient, at 1.3 m 49 %.
  The structure is a top-level one over the scene's static BLAS alone, built for this pass: characters receive probe light,
  they do not cast into it (the probes are static). Batches of 4096 probes, each submitted and waited for.
- **Storage:** the per-thread sums are reduced in groupshared memory (no atomics) to L1 SH already convolved with the
  cosine lobe: `E(n) = c0 + c1 n.x + c2 n.y + c3 n.z`, `c0 = pi mean(L)`, `c1..3 = 2 pi mean(L w)` (`ProjectProbeSh`).
  Four RGBA16F 3D textures, one per coefficient. Alpha of `c0`: validity (0 when more than 30 % of the rays hit the back
  of an opaque triangle, by its vertex normal; alpha-cut cards never count). Alpha of `c1..3`: the probe's free distance
  along +axis and -axis (six axis rays up to one spacing, 31 steps each, packed `32 q(+) + q(-)`, exact in FP16).
- **Lookup** (`PW_PROBES`, `shaders/raster_probes.hlsli`, in `RasterShade` only): the point moves 0.2 spacing along the
  normal; the 8 surrounding probes weigh trilinear x validity x side x visibility, renormalised. Side: 1 in front of the
  surface, 0 from 0.1 spacing behind it. Visibility: 0 when the point lies beyond the corner probe's free distance along an
  axis (in that direction), which stops the leaks through thin geometry that the side test cannot see (the lab's roof is
  one layer between two probe layers: without it the probes above the roof, at 40x the hall's ambient, lit the ceiling
  beams, and a pillar edge got a bright strip). Without a corner in front and in sight, trilinear x validity; without a
  valid corner, or outside the grid, the fallback: the sky's SH with `--raster-sky-ambient`, else the constant. The ambient
  is `albedo * max(E(n), 0) / pi`, as the sky's; `--ao` scales it as before. With `--lightmap` baked static pixels keep
  their baked light. The fog's ambient grey stays `--raster-ambient`.
- **Binding:** one descriptor table, `b0 space5` (grid constants) and `t0..t3 space5` (the textures), appended after the
  sky's CBV in every scene root (`Raster::probeParameter`); space 5 is used by no other root. 16 pixel shaders
  `raster_probes[_sky][_ao][_csm][_lightmap]_ps.cso` and `probes_trace.cso`; the 40 earlier `.cso` are byte-identical,
  and a run without the flag is unchanged (the yard f990 `--csm --raster-sky-ambient` dump has Task 2's hash).
- **Cost** (RTX 4080 SUPER): lab trace 7 ms, startup 190-480 ms (the TLAS, the PSO, the readback); yard trace 30 ms,
  startup 215 ms. Frame at 3840x2160: lab raster 9.7 ms (the earlier acceptance run) -> 11.2 ms, yard `--csm --raster-sky-ambient` 10.35 -> 11.07 ms.
- **Results** (`<bench root>\look\18_probes_*`): mean luminance / path tracing, lab snap 0, 3, 30 and combo 90: plain
  0.83, 0.85, 1.16, 0.84; probes 0.82, 0.83, 1.01, 0.85. Yard f990: constant 0.88, sky 0.95, probes 0.94. Indoors the probes are warm (the lamps' and
  floor's bounce), outdoors sky-blue; the hall interior in the yard scene keeps its own light instead of the sky's.
- **Limits:** L1 cannot hold a small bright source: the opposite side clamps to 0. A character's back reads darker than
  with the constant, which hid raster's missing direct lamp light (one unshadowed point per lamp group): the probe at the
  lab character sees almost nothing behind her, with the lamps on or off, while path tracing lights her from the tubes
  (`--lamp-tubes` gives raster that direct light). The grid is traced once: lamp changes
  in the menu do not reach it. The visibility test follows the axes only, so diagonal thin geometry can still leak a little.

## Lamp tubes (`--lamp-tubes`)

Without it each lamp group is one unshadowed point light at its centroid with the group's power: the lab's fifteen-odd tubes
of a group light the hall from one point in the middle, so the characters under the tubes miss the light from above that
path tracing gives them (their backs and shoulders read dark), and the polished floor shows one dot per group. The probes
cannot add it: they hold the lamps' bounce light, and the direct light would count twice.
- **Tubes** (`lamp_parts.h/.cpp`, once at start): the emitters of each group split into connected parts (triangles sharing
  a vertex position to 0.1 mm). Each part's principal axes are the eigenvectors of its surface's area-weighted second
  moment; its extents are the bounds of its corners along them. A part is a tube when it is at least 4 times longer than
  its wider cross extent and that one is at least 2 times the thin one (a dominant face): a segment from one end of its
  bounds to the other through its centre, radius half the mean cross extent. Everything else stays compact (screens,
  spots, the ring's square-section segments, short signs), as the group's point light. Lab: 90 parts in 6 groups, 30
  tubes (0.7-3.0 m: the bay and nave fixtures, the pendants, the long signs), 60 compact in 4 point lights.
- **Faces.** The thin cross axis is the emitting face's normal. Within 45 degrees of vertical the tube is a ceiling fixture:
  one-sided, emitting downward, and nothing above the plane of its face is lit (its housing hides the upper face and the
  sides). Otherwise (a standing sign) it emits from both faces. The tube is a box of Lambertian faces of its radiance: its
  intensity toward a direction is `P (faceShare max(cos theta, 0) + sideShare |cos phi|)` (`|cos theta|` two-sided),
  theta to the face normal, phi to the side axis (segment x normal), P the tube's power `sum(area * emission)` times the
  group's intensity and tint, faceShare and sideShare the areas of one face (length x wide extent) and one side
  (length x thin extent) over the part's area. A lab bay fixture (2.4 x 0.18 x 0.06 m): 0.37 and 0.12. Its top face and end
  caps send nothing. Each group's compact parts keep one point light, built from them alone as before (a group without
  tubes keeps its light unchanged).
- **Diffuse** (`shaders/raster_tubes.hlsli`, mirrored on the CPU by `LineIrradiance`/`TubeIrradiance`): the face and side
  normals are perpendicular to the segment, so `cos * r` is the same for its whole length and both share one integral: the
  segment clipped to the surface's horizon, then the exact mean of `n.l / r^4` over it in closed form
  (`(n.h + s n.u) / (h^2 + s^2)^2`, h the offset to the axis, s along it), held at the tube radius from the axis. Far away
  it is the point source of those faces (`tests_tubes.cpp`, within 2 %). The Fresnel term of the diffuse lobe takes the
  direction to the clipped segment's middle.
- **Specular**, UE4's representative point: the point of the segment closest to the reflected ray, then the point of a sphere
  of the tube's radius around it closest to the ray; Bsdf's specular toward it with the tube's intensity toward it at its
  distance, times `alpha / alpha'` for the length and `(alpha / alpha'')^2` for the radius (`alpha = roughness^2`,
  `alpha' = saturate(alpha + 0.5 length / d)`, `alpha'' = saturate(alpha + 0.5 radius / d)`, d the distance to the
  closest point). On the polished floor each tube becomes a streak along it.
- **Range:** where a tube's irradiance falls to 0.5 % of its value at 1 m by the inverse square, `sqrt(1 / 0.005)` = 14.1 m
  from the segment (`TubeRange`). The shader skips a tube when the surface point is farther from its segment, or above a
  one-sided tube's face; between 0.75 and 1 range it fades it out (a smoothstep, `TubeRangeWindow`), so no edge shows.
- **Depth prepass:** without `--ao` the scene pass would evaluate the tubes for every drawn fragment; a depth-only prepass
  (`raster_tubes_prepass[_lightmap]_ps.cso`, the scene pass's own vertex shader, the alpha cutoff) runs first and the scene
  pass tests EQUAL, so each pixel is shaded once. With `--ao` its prepass does it.
- **Where:** in `RasterDirectLight`, after the point lights, so in `RasterShade` (`--ao` occludes the tubes' diffuse as the
  point lights') and, with `--lightmap`, as specular alone on baked surfaces (their diffuse is in the bake). Unshadowed, as
  the point lights. The probes are unchanged.
- **Binding:** the lines (80 bytes each, `RasterLine` in `raster_lights.h`) are a root SRV at `t0 space6` and their count
  one root constant at `b0 space6` (space 6 is used by no other root), appended after the probes' table in every scene root
  (`Raster::tubeParameter`). 32 pixel shaders `raster_tubes[_probes][_sky][_ao][_csm][_lightmap]_ps.cso` (`PW_TUBES=1`,
  every combination a command line reaches: sky, probes, `--ao`, `--csm`, `--lightmap`) and the two prepass shaders; the 57
  earlier `.cso` are byte-identical and a run without the flag is unchanged.
- **Results** (RTX 4080 SUPER, 3840x2160, `<bench root>\look\18_tubes_*`). Mean luminance / path tracing (rr):

  | | snap 0 | snap 3 | snap 30 | combo 90 |
  | --- | --- | --- | --- | --- |
  | `--probes` | 0.82 | 0.83 | 1.01 | 0.85 |
  | `--probes --lamp-tubes` | 1.01 | 0.86 | 0.97 | 0.92 |
  | baked `--csm --probes` | 0.88 | 0.89 | 1.09 | |
  | baked `--csm --probes --lamp-tubes` | 1.05 | 0.89 | 1.08 | |

  The characters' backs and shoulders are lit from above as in path tracing and the floor shows a streak per tube. The
  first version (isotropic tubes, no range, no prepass) gave 1.30, 1.17, 1.84, 1.05: it lit the ceiling and the upper
  pillars that the housings shadow in path tracing.
- **Cost:** raster passes (the `path trace` figure) lab snap 5.1 ms with `--probes` alone, 7.7 ms with the tubes; combo
  7.0 ms; baked `--csm` 7.3 ms (debug layer). The first version took 21.5 ms.
- **Checks:** `tests_tubes.cpp` (the axis, length, radius and face of a flat strip and of a rotated fixture, a two-sided
  sign, compact spots, screens and square rods, parts per group, the power split and the shares, the closed form against
  a numeric sum one- and two-sided with and without the horizon, nothing above the face, the far field, middle vs end of a
  long tube, the radius hold, the range fade, the option). Debug layer (baked `--csm --probes --lamp-tubes`): 0 errors,
  the 12 known "Ignoring InitialState" warnings.

## Lamp shadows (`--lamp-shadows`)

Without it the lamps are unshadowed: the floor under a desk and under a character is as bright as the open floor, and with
`--lightmap` a character casts no lamp shadow at all on baked surfaces. `--lamp-shadows` gives every lamp group a spot shadow
map, as Unreal's stationary lights: the static shadows of the baked lamp light are in the bake, the dynamic ones come from the
map.
- **One map per group** (`raster_lamp_shadows.h/.cpp`, `FitLampShadows`, once at start): 1024², a perspective frustum looking
  straight down. A group is one light in raster (its point light, and with `--lamp-tubes` its tubes), so one map covers the
  whole group, tubes included.
  - The lit area: the emitters' footprint plus a 60 degree spread from their height above the floor (the scene's lowest y),
    radius `e + sqrt(3) h` (e the footprint's half diagonal, h the power-weighted centroid's height).
  - The apex: above the footprint's centre, e above the centroid. A lone lamp (a yard pole's head) has it at the lamp and a
    120 degree field of view (the yard's poles: 119 degrees); a field of tubes (the lab's `lab_tube`, 16 x 21.5 m) has it
    high above the ceiling (17.4 m), so its shadows fall steeper, nearer the field's average light (100 degrees). The field of view
    is `2 atan(radius / apex height)`, 90..120 degrees.
  - The near plane is 5 cm below the lowest emitter, so the lamps, their housings and the ceiling never cast; the far plane
    is the floor.
  - At most 16 groups (the array's slices); more get no map, with a warning. Lab: 6 maps; the yard and the garden: 9.
- **Drawing** (`raster_lamp_shadow_passes.cpp`, every frame, before the sun's maps): each slice cleared and drawn with every
  triangle (`raster_lampshadow_vs.cso` and the sun pass's alpha-cutoff pixel shader, slope-scaled bias 1.5). Static casters
  are drawn again every frame; caching them would save the pass's cost (below).
- **Lookup** (`shaders/raster_lamp_shadow.hlsli`, `PW_LAMPSHADOW`): `RasterDirectLight` first tests every group's map at the
  surface (3x3 PCF as the sun's; the point moved 1.5 texels along the geometric normal and one texel toward the apex, texels
  at the point's view depth) and multiplies each point light and tube by its group's result (`PW_LAMP_VISIBILITY`; the slice
  of every light in a buffer written with the lights). Outside the frustum: lit.
  - A surface facing away from the apex is not tested, and the test fades in up to a cosine of 0.2: one map stands for a
    whole group, and a surface turned away from its apex (a character's back under a field of tubes, a pillar's far side)
    is lit by the group's other emitters, which the map cannot see; tested, it would be shadowed by the surface itself.
  - So the lamps' diffuse and specular are shadowed in raster, and in baked mode on dynamic pixels (characters, foliage) and
    the lamps' specular on static ones.
- **Baked mode** (`--lightmap`, [RASTER_BAKED.md](RASTER_BAKED.md)): the bake has a third map, `lamp`, the lamp groups' direct
  light; `rest` is then the indirect light and the sky. A second array holds the triangles without baked light alone
  (characters, and static triangles without lightmap UVs such as the garden's foliage: `Scene::unbakedTriangles`,
  `raster_lampshadow_list_vs.cso`). Static pixels:
  `albedo * ((rest + lamp * lampVis) * scaleFull + sun * scaleSun * dynShadow) + emission + specular`.
  - `lampVis` (`LampVisibility`) is each group's test against the second array, averaged with the group's luminance power
    times its point light's falloff at the pixel (`LampFalloff`) as the weight: a character's lamp shadow darkens only the
    lamps' direct light, never the bounce.
  - It needs a lightmap with `"lamp"`: `Cannot open lightmap <file>: --lamp-shadows needs a lightmap with "lamp" maps`.
    A lightmap with `"lamp"` without the flag loads `rest + lamp` (summed before the packing) as the first map, so the
    light is the bake's `full - sun`, as before.
- **Binding:** space 7, used by no other root. Scene roots (`AddAmbientParameters`, after the tubes' parameters,
  `Raster::lampShadowParameter`): a table of the two arrays (`t1`, `t2 space7`; a null view for `t2` without `--lightmap`),
  the constants (`b0 space7`: per slice the view-projection, the apex and texel scale, the centroid and luminance power; the
  count and the first lamp atlas), the light slices (`t3 space7`). The pass's root: graphicsRoot's parameters, the constants,
  the triangle list (`t2 space3`) and the slice drawn (`b1 space7`). The baked root's parameter 10 is that triangle list too
  (`t2 space3`, also the sun's list draws below). Free after this: `t0`, `t4+` and `b2+` in space 7, `t3+` in space 3.
- **Shaders:** `raster_lampshadow_vs.cso`, `raster_lampshadow_list_vs.cso` and 64 scene pixel shaders
  `raster_lampshadow[_tubes][_probes][_sky][_ao][_csm][_lightmap]_ps.cso` (`build.cmd`'s `:lampshadow_ps`). The 91 earlier `.cso`
  are byte-identical.
- **The baked mode's dynamic sun shadow and foliage.** Independent of the flag: in baked mode the dynamic-only sun map (and its
  cascades) holds every triangle without baked light. Before, it held the dynamic range alone, so static foliage without
  `--wind` (no lightmap UVs, not in the bake) cast no sun shadow on baked surfaces. When a scene has such triangles the
  dynamic-only draws walk `Scene::unbakedTriangles` (`raster_shadow_list_vs.cso`, `raster_csm_shadow_list_vs.cso`, on the
  baked root); otherwise (the lab, the yard) they are drawn as before.
- **Results** (RTX 4080 SUPER, 3840x2160, `<bench root>\look\18_lampshadow_*`). Mean luminance / path tracing (rr):

  | | snap 0 | snap 3 | snap 30 | combo 90 |
  | --- | --- | --- | --- | --- |
  | `--probes --lamp-tubes` | 1.01 | 0.86 | 0.97 | 0.92 |
  | `--probes --lamp-tubes --lamp-shadows` | 0.97 | 0.83 | 0.94 | |
  | baked `--csm --probes --lamp-tubes` (bake of 2026-10-05) | 1.05 | 0.89 | 1.08 | |
  | baked `--csm --probes --lamp-tubes --lamp-shadows` (lamp-split bake) | 1.05 | 0.89 | 1.13 | 0.92 |

  Raster: the floor under the desks and under the characters is dark, as in path tracing, and the tubes' reflections on the
  floor are cut where a desk stands between them and the apex. The shadows are sharper and darker than path tracing's:
  one map stands for the whole group, so a pixel it shadows loses every tube's light at once, where path tracing keeps
  the other tubes' light and a wide penumbra. Baked: the lamp map is about a quarter of the lab's non-sun baked light, so
  the characters' lamp shadows on the baked floor are faint. Garden 990 (old bake, no flag): the trees and the grass
  now shadow the baked ground.
- **Cost:** raster passes (the `path trace` figure) lab snap 7.7 ms -> 10.7 ms with 6 maps (frame 15.7 -> 17.3 ms); baked
  `--csm --probes --lamp-tubes` 5.6 ms (old bake, no flag) -> 9.6-9.8 ms with the lamp-split bake and the flag.
- **Checks:** `tests_lamp_shadows.cpp` (the fit covers the lit area's rim on the floor, the emitters lie in front of the near
  plane, 90..120 degrees, the slices per group and the 16-map cap, the weighted visibility is 1 with no casters and favours
  the nearer and stronger group, the raster lights' groups, the unbaked list, the JSON with and without `"lamp"`, the option).
  Identity: a baked run without the flag on the 2026-10-05 bake writes Task 2c's dumps byte for byte. Debug layer (baked
  lab with the flag; the garden's list draws): 0 errors, only "Ignoring InitialState" warnings.
- **Limits:** one map per group, so the shadows of a wide group fall from one apex (in the lab's hall about 30 degrees off
  vertical at the edge of the tube field) and are as dark as if the whole group were behind the occluder; a softer
  filter (PCSS or a wider kernel per group) would bring them nearer path tracing. Static casters are redrawn every frame.
  Only casters below the lowest emitter of a group cast for it.

## FXAA (`--fxaa`)

A game's own anti-aliasing runs before its post chain; `--fxaa` gives raster that: FXAA 3.11, quality preset 12, filtered inside
the raster resolve, so the upscaler, NR and the add-on receive the anti-aliased frame. Valid only with `--render raster`
(`--fxaa requires --render raster`); it composes with every other raster option (it only reads the raster targets). Without the
flag nothing is created and nothing changes.
- **Interactive:** the menu's Render section has an "FXAA" checkbox (raster only, hidden in path tracing), initialised from `--fxaa`
  or the saved settings. `Raster::Record` picks the resolve pipeline from `Options::fxaa` every frame, so a click takes effect on the next
  frame; the FXAA pipeline is created on first use (`--fxaa` creates it before the first frame). The state is saved in `settings.json`
  (`fxaa` "0"/"1", a bare flag like `hud`); a saved `fxaa` is ignored, not rejected, in a path-traced start (only an explicit
  `--fxaa` needs raster).
- **Where:** `raster_resolve.hlsl` built with `PW_FXAA=1` as `raster_fxaa_resolve.cso`; `Raster::ResolvePso` (`raster_fxaa.cpp`) swaps
  the resolve's pipeline by `Options::fxaa`. No target, descriptor or pass is added, and `raster_resolve.cso` (and the 158 other
  earlier `.cso`) are byte-identical. `--hud` is blended after the resolve, so the HUD is not filtered (as in a game); in
  `--view accum` and the accumulated copy the filtered colour is what is accumulated.
- **What is filtered** (`shaders/raster_fxaa.hlsli`): every tap is the frame's colour of that pixel as the resolve writes it (the
  shaded surface or the sky, then fog and the firefly clamp), not the raw colour target. The target holds no sky and no fog, so
  filtering it alone would blend a silhouette against black and miss the sky and fence-against-sky edges; this is the colour a
  game's AA sees, fog being part of its scene shading. Taps outside the frame clamp to the edge.
- **Luma:** the colour is linear HDR with exposure applied, FXAA's thresholds are tuned for gamma-encoded LDR. The luma is of a
  tonemapped estimate, `l / (1 + l)` (the present pass's neutral curve), square-rooted as a cheap sRGB encode. Only the edge
  detection and search use it; the blended colour is linear (bilinear taps, as the hardware filter of the reference).
- **Filter:** Lottes' reference: early exit below `max(0.0833, 0.166 * maxLuma)`, edge direction from the 3x3 luma, end-of-edge search
  of 5 steps (1, 1.5, 2, 4, 12 px), sub-pixel blend 0.75. A pixel that is not on an edge costs 5 taps (the centre and four
  neighbours, each a depth read plus fog, and the sky where nothing was drawn).
- **Results** (RTX 4080 SUPER, 3840x2160, `<bench root>\look\18_fxaa_*`): the stair-steps on the hall's diagonal rails, tube edges
  and the character's hair, and in the garden on the fence and tree leaves against the sky and on the grass cards (alpha-cutoff
  edges), are smoothed; textures stay as sharp (no change away from edges). Lab snap 0: 4.9 % of the pixels change, 1.1 % by more
  than 8/255; garden (close view) 16.9 % / 6.8 %; garden combo 990 32.4 % / 13.9 % (the grass cards). Mean linear luminance +0.5 % to
  +1.5 % (blending in linear light).
- **Cost:** below the noise of the frame time at 4K. Lab raster snap, 3 runs of 121 frames each: raster passes 2.57, 2.86, 3.15 ms
  without and 2.83, 2.69, 2.76 ms with the flag (frame 9.78-10.20 vs 9.83-10.03 ms); garden baked combo 990: 16.9 ms without and 17.0
  ms with it (debug layer on). So well under 0.3 ms.
- **Checks:** `tests_fxaa.cpp` (off by default, rejected without raster and with `--render pathtrace`, composes with `--csm --ao game
  --probes --lamp-tubes --lamp-shadows --wind --hud --upscaler sr`). Identity: a run without the flag writes the byte-identical
  dump of the build before it (lab snap 0, PNG SHA256 686E4022...). Debug layer (lab `--csm --ao game --probes --lamp-tubes
  --lamp-shadows --hud --fxaa`; baked garden combo 990 `--fxaa`): 0 errors, only "Ignoring InitialState" warnings (11 and 13).
- **Limits:** the shader is FXAA, not temporal AA: thin geometry that breaks into sub-pixel dots (distant grass, wires) is only
  softened, not recovered, and moving content still shimmers; the sub-pixel term (0.75) softens high-contrast single-pixel detail
  slightly. The filter reads taps of the finished frame, so it does not know about the upscaler's jitter: with `--upscaler sr` the
  jittered frame is filtered, then upscaled.

## Wind (`--wind`)

Foliage that sways, for testing frame carry and temporal filters on motion that is not the camera's. The geometry is moved
in the characters' animation path (the GPU skinning pass, INSTANCING.md "GPU skinning"; `--cpu-skinning`: the CPU workers), so path tracing and raster see the same vertices and the motion vectors (the
previous positions) are exact. Without `--wind` nothing is dynamic about the foliage: dumps and `.cso` are unchanged.
- **Which nodes:** mesh nodes whose name starts with `veg_` (`MarkWindNodes`, `wind.cpp`) get `Node::wind`, which makes
  `DynamicNode` true (`tools/bench/pw_gltf_vertices.h`; the flag stays false in the D3D11 bench). They join the dynamic
  triangles (second BLAS geometry, refit every frame, vertex upload every frame). With none: `[warn] --wind: the scene has
  no veg_ nodes, wind has no effect`.
- **Clock:** the bench frame index, `t = frame / 60 s`, passed through `Scene::Animate` into `Animation::Evaluate`. It is
  independent of `--anim-playing`, `--anim-speed` and the characters: wind moves with the animation frozen. The previous
  offset is the one of frame - 1 (frame 0: its own, so no motion), so a vertex's `prevPos` equals the `pos` it had a frame earlier.
- **Displacement:** along (0.8, 0, 0.6) in bench space, `S * w * (0.08 sin(wt + k p) + 0.03 sin(2.7 wt + 1.3 k p))`, with
  `w` = 2 pi * 0.6 Hz, `k` = 0.35 / m and `p` = dot(rest position x/z, wind), so at most 0.11 S m. Normals are not changed.
  The path tracing accumulation restarts every frame (`Scene::windActive`).
- **Weights** (`WindWeights`, once per node at start, from the rest geometry in bench space):
  - a material named `*trunk*`, `*bark*` or Poly Haven's bare `island_tree_NN` (the tree's trunk): 0;
  - a node whose name contains `grass`: per quad (vertices linked through the triangles), the top half of the quad 1 and
    the bottom half 0, so a single merged grass mesh works;
  - any other `veg_` node (trees, ferns): (height above the node's lowest vertex / the node's height)^2. One node per
    tree or fern.
- **Probes:** dynamic geometry is a receiver only, so with `--probes` the `veg_` nodes leave the probe TLAS.
- **Cost:** 2 offsets per wind vertex per frame in the GPU skinning pass (the CPU only computes the frame's two sines); with
  `--cpu-skinning` on the animation workers (reported in `[info] cpu animation`, about 4 ms in the garden).
- **Checks:** `tests_wind.cpp` (weights, the formula, the dynamic set with and without `veg_` nodes, the vertices
  `Animation` writes at several frames with the animation time fixed). The lab (no `veg_`) with and without `--wind` writes
  byte-identical dumps.

## Instanced meshes

Meshes shared by static nodes are stored once and drawn per instance in every raster pass (culled per pass on the CPU),
traced through a BLAS per mesh: [INSTANCING.md](INSTANCING.md). `--no-instancing` flattens them as before. The characters and
wind nodes are skinned on the GPU (INSTANCING.md, "GPU skinning"; `--cpu-skinning` for the CPU path). Small instances (bounding
sphere below `--cull-small-radius` 0.75 m) leave every pass outside a `--cull-cone-deg` 170 cone around the view, and the camera
passes also drop those the Hi-Z of the frame's occluders hides (INSTANCING.md, "Culling small objects"; `--no-small-culling`,
`--no-occlusion-culling`, `--cull-check`, `--cull-debug`).

## Source files

- `frame_constants.h`: the per-frame constants (b0) both renderers read.
- `raster.h/.cpp`, `raster_resources.cpp`, `raster_pipeline.cpp`: `--render raster` (per-frame passes, targets and
  lights, root signatures and PSOs) and the `--motion none` pass.
- `raster_shadow.h` fits the sun shadow map; `raster_lights.h/.cpp` turn lamp groups into point lights (and, with
  `--lamp-tubes`, their tubes into line lights); `lamp_parts.h/.cpp` find the tubes; `raster_tubes.cpp` uploads the lines.
- `shaders/raster.hlsl`, `raster_shadow.hlsl`, `raster_resolve.hlsl`: the scene and shadow passes, the resolve and
  zero-motion.
- `shaders/raster_common.hlsli`: shared lighting and fog. `shaders/raster_depth.hlsli`: the hardware-depth inversion,
  shared with the CPU checks.
- `hud.h/.cpp`, `hud_layout.h`, `shaders/hud.hlsl`: `--hud` (the compute pass, the layout shared with the CPU checks,
  and the blend shader with its 5x7 font).
- `camera_snap.h`: the snap camera's yaw curve. `tests_raster.cpp`: the CPU checks of all of the above.
- `--lightmap` (the workflow and the mode: [RASTER_BAKED.md](RASTER_BAKED.md)):
  - `lightmap.h`: the declarations;
  - `lightmap_file.cpp`: the JSON, the sun and lamp warnings and the R9G9B9E5 encoding (CPU only, linked by the checks);
  - `lightmap.cpp`: loads and uploads the atlases;
  - `lightmap_uv.h`: the per-vertex lightmap UVs;
  - `raster_lightmap.cpp`: the baked mode of `Raster` (its pipelines are in `raster_pipeline.cpp`, its shadow map in
    `raster_resources.cpp`);
  - `shaders/raster_lightmap.hlsli`: the baked light.
- `--csm`: `raster_csm.h` (the cascade fit, CPU, shared with the checks), `raster_csm.cpp` (the arrays, the cascade
  draws, the constants; its pipelines are in `raster_pipeline.cpp`), `shaders/raster_csm.hlsli` (cascade selection,
  blend and PCF).
- `--raster-sky-ambient`: `sky_sh.h` (the SH9 projection and evaluation, CPU, shared with `tests_sky_sh.cpp`),
  `raster_sky.cpp` (`Raster::UseSkyAmbient`: loads the HDRI, writes the constants; its pipelines are in
  `raster_pipeline.cpp`), `PW_SKYAMB` in `shaders/raster_common.hlsli`.
- `--probes`: `probes.h` (the layout, the L1 projection and evaluation, the corner weights and the visibility packing, CPU,
  shared with `tests_probes.cpp`), `probes.cpp` (`BuildProbes`: the static TLAS, the trace, the readback, `--probe-dump`),
  `raster_probes.cpp` (`Raster::UseProbes`: the constants and views; its pipelines are in `raster_pipeline.cpp`),
  `shaders/probes_trace.hlsl`, `shaders/raster_probes.hlsli` (`PW_PROBES`, included by `raster_common.hlsli`).
- `--ao`: `raster_ao.h/.cpp` (`RasterAo`: the targets and the AO passes; `Raster`'s prepass and binding; its pipelines
  are in `raster_pipeline.cpp`), `ao_math.h` (the depth and normal helpers, CPU, shared with the checks) and its HLSL
  mirror `shaders/raster_ao_math.hlsli`, `shaders/raster_prepass.hlsl`, `shaders/raster_ao.hlsl`,
  `shaders/raster_ao_temporal.hlsl` (`--ao game`).
- `--wind`: `wind.h/.cpp` (the weights and `WindOffset`, CPU, shared with `tests_wind.cpp`); the vertices are written by `Animation::Process` (`animation.cpp`).
- `--lamp-shadows`: `raster_lamp_shadows.h/.cpp` (the frusta and the baked visibility weights, CPU, shared with
  `tests_lamp_shadows.cpp`), `raster_lamp_shadow_passes.cpp` (`Raster::UseLampShadows`: the arrays, the constants, the light
  slices, the draws, the binding; its pipelines are in `raster_pipeline.cpp`), `shaders/raster_lamp_shadow.hlsli`
  (`PW_LAMPSHADOW`, included by `raster_common.hlsli`), `PW_LAMPSHADOW` and `PW_SHADOW_LIST` in `shaders/raster_shadow.hlsl`.
- `--fxaa`: `raster_fxaa.cpp` (`Raster::UseFxaa`: swaps the resolve's pipeline), `shaders/raster_fxaa.hlsli` (`PW_FXAA`, included by
  `shaders/raster_resolve.hlsl`), `tests_fxaa.cpp`.
