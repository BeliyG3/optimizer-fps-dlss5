# pw_bench12 raster sky reflections (`--sky-reflections`) and AO strength 1

The raster frame had no specular from the sky: only the sun and the lamps reflected. Path tracing's lacquered benches, glossy
props and walls reflect the sky, so shaded vertical and glossy surfaces read about half of path tracing (task 5c: the bench in
shade 0.47, the wall in shade 0.55). `--sky-reflections` adds the sky's image-based specular as games add their sky light's:
a prefiltered environment cube and the split sum's DFG table, occluded by the AO and by the probes' sky visibility. Part of
[RASTER.md](RASTER.md); measurements in "Results" below.

| Option | Default | Meaning |
| --- | --- | --- |
| `--sky-reflections 0\|1` | 1 with `--hdri`, else 0 | Raster and baked mode: the sky's specular on every surface. The menu's Render section has a "Sky reflections" checkbox (raster only) that flips it live; `settings.json` keeps it as `sky-reflections` (`-1`: the default). Path tracing ignores it (it reflects the sky by itself). |
| `--ao-strength S` | `1` (was `1.5`) | The AO visibility is raised to this power (RASTER.md, "Ambient occlusion"). Below: why 1 everywhere. |

Without `--hdri` the sky is the bench's constant sky colour (0.12, 0.16, 0.23), so `--sky-reflections 1` reflects that colour; it
is off by default there, and every run without it writes the frames it wrote before (checked below).

## The prefiltered cube (`raster_reflections.cpp`, `shaders/sky_prefilter.hlsl`)

Built once at start on the GPU, in every run (a few milliseconds; the resources exist even when the option is off, so the menu
can turn it on):
- **Source:** the scene's sky panorama (`Scene::environment`, the `--hdri` file or the constant) as a 256² cube, each texel the
  mean of 4x4 bilinear taps of the panorama through `SkyUv` (`shaders/sky_uv.hlsli`), the mapping `Sky()` uses for the path
  tracer's misses and the raster sky, so the orientation and scale are the trace's. Then its mip chain (2x2 boxes) down to 1².
- **Prefilter:** a 128² cube (UE's default sky light size) with 6 mips; mip m holds roughness m / 5 (0, 0.2, ..., 1). Mip 0 is
  the source at its texel's footprint (the mirror); mips 1..5 are Karis' split-sum prefilter with N = V = R: 512 GGX samples
  (Hammersley) of the reflected direction weighted by n.l, each read from the source mip whose texel matches the sample's solid
  angle (filtered importance sampling, + 1 mip), so the result has no sampling noise.
- **DFG table** (`sky_reflections.h`, on the CPU): 32² texels of (A, B), n.v across and roughness down at texel centres, 1024
  GGX samples each: the integral of the bench's own specular lobe (`Bsdf()` in `shaders/bsdf.hlsli`: GGX with a2 = roughness^4,
  the separable Smith G1 of both directions, Schlick's Fresnel of v.h) times n.l, split as `f0 * A + B`.
- The math is one file, `shaders/sky_reflection_math.hlsli`, read by the prefilter pass, the scene pass, the CPU table and the
  checks (through `hlsl_compat.h`).

## Shading (`shaders/raster_reflections.hlsli`)

`SkyReflection` in `RasterShade` (raster mode, and baked mode's dynamic and unbaked pixels) and in `LightmapShade` for baked
static pixels (the bake holds no specular, so nothing counts twice):

`prefiltered(reflect(view, n), lod = roughness * 5) * (f0 * A + B) * specularOcclusion * skyVisibility`

- `f0`, `roughness` and `n` are the surface's (`GetSurface`, the materials the trace and the raster direct light use): 0.04 for
  dielectrics, the base colour for metal.
- **Specular occlusion** from `--ao` (Lagarde and de Rousiers 2014): `saturate((n.v + ao)^(2^(-16 alpha - 1)) - 1 + ao)`,
  alpha = roughness²: a rough lobe is occluded about as much as the diffuse light, a smooth one seen head-on hardly at all.
  1 without `--ao`.
- **Sky visibility** from `--probes` (below), in the reflected direction, rescaled from 0.15 (0) to 1 (1)
  (`ReflectionVisibility`, `SkyVisibilityFloor`); 1 without `--probes` or outside the grid. L1 cannot hold a closed room with a
  few openings: it rings to about 0.1-0.25 everywhere inside the hall, and the sky is 10-50 times brighter than the hall, so the
  raw value made the hall 4 % brighter and its pillars 12 % (raster / path tracing 0.93 -> 0.97, 1.09 -> 1.22). The floor removes
  the ringing (the hall as without reflections) and keeps open sky open. Squaring instead (UE's sky occlusion exponent 2) also
  fixed the hall but took the shaded bench and wall down to 0.74 of path tracing.
- Before exposure, added to the ambient term; the sun's and lamps' specular stay as they were (they already reflect).

## The probes' sky visibility (`probes.h`, `shaders/probes_trace.hlsl`, `shaders/raster_probes.hlsli`)

Each probe already traces `--probe-rays` directions once at start. Each of them now also traces a visibility ray that finds out
whether it escapes to the sky: against the scene's whole structure, so `--wind`'s foliage (dynamic, at its start pose) blocks
the sky, while the characters (dynamic too, but not where they walk later) never do; triangles of `--wind` nodes carry the
material flag 8 for this (`scene.cpp`). The escape indicator e is stored in L1 like the irradiance, in a fifth RGBA16F texture:
`v0 = mean(e)`, `v1..3 = 3 mean(e w)`, and `V(d) = saturate(v0 + v1..3 . d)`. Open ground: 1 up, 0.5 at the horizon, 0 down;
inside the hall about 0 everywhere; beside a wall, open away from it and closed into it. The lookup interpolates the four values
with the irradiance's corner weights (trilinear x validity x side x visibility, `ProbeAmbient`), so it never leaks where the
irradiance does not. The startup log reports it: `[probes] sky visibility of the valid ones: X % of the rays escape on average,
Y % enclosed`. `--probe-dump` writes `PWPROBE2` (20 floats per probe: the four coefficients, then the visibility).

## AO strength 1

The owner asked for 1.0 outdoors and 1.5 indoors kept, blended by the probes' sky visibility, or 1 everywhere if the hall shows no
reason for the blend. Measured (raster / path traced, mean linear luminance, fixed exposure, the hall view of task 5c):

| hall | frame | floor | left desk | pillar |
| --- | --- | --- | --- | --- |
| AO 1.5 (before; what the blend keeps indoors) | 0.92 | 0.92 | 1.13 | 1.09 |
| AO 1 | 0.93 | 0.92 | 1.17 | 1.09 |
| AO 1.5, with the reflections | 0.93 | 0.92 | 1.13 | 1.09 |
| AO 1, with the reflections (the default now) | 0.93 | 0.92 | 1.17 | 1.09 |

The hall moves by 1 % (the frame closer to path tracing, the desk 4 % further), well inside what the blend would buy, while the
blend would need the AO pass to keep its raw visibility and the scene pass to raise it per pixel (a second exponent after the
blur, and the probe lookup in every AO pixel). So `--ao-strength` defaults to 1 everywhere; `--ao-strength 1.5` brings the old
look back. Outdoors AO 1 alone takes the bench in shade from 0.47 to 0.54 and the wall from 0.55 to 0.58 of path tracing; the
rest comes from the reflections.

## Results

Raster / path traced (`--upscaler rr`), mean linear luminance of task 5c's regions, fixed exposure, ACES, 1920x1080, frame 180,
the owner's flags (`--csm --ao clean --probes --lamp-tubes --lamp-shadows --raster-sky-ambient --hdri ... --wind --fxaa`).
Before: the HEAD build (e0ef42b, AO 1.5). Floor 0: reflections with the raw visibility (AO 1). Final: with the 0.15 floor
(the hall, path and shade views ran the final scene shader as an experiment, byte-identical to the final build's).

| view, region | before | AO 1 only | reflections, floor 0 | final |
| --- | --- | --- | --- | --- |
| path: bench in shade | 0.47 | 0.54 | 0.98 | **0.88** |
| path: wall in shade | 0.55 | 0.58 | 0.87 | **0.82** |
| path: sand in sun / sky / frame | 0.99 / 0.99 / 0.98 | 0.99 / 0.99 / 0.98 | 1.00 / 0.99 / 1.01 | 1.00 / 0.99 / 1.00 |
| path: sand in shade | 1.09 | 1.11 | 1.31 | 1.29 |
| shade: sunlit ground / tree shadow / sunlit wall | 1.01 / 1.02 / 1.00 | 1.02 / 1.03 / 1.00 | 1.02 / 1.04 / 1.02 | 1.02 / 1.04 / 1.01 |
| shade: hall seen from outside | 0.78 | 0.79 | 0.95 | 0.90 |
| hall: frame / floor / desk / pillar | 0.92 / 0.92 / 1.13 / 1.09 | 0.93 / 0.92 / 1.17 / 1.09 | 0.97 / 0.92 / 1.18 / 1.22 | 0.93 / 0.92 / 1.17 / 1.09 |
| picnic: frame / sunlit sand / table / white suit | 0.97 / 0.98 / 0.98 / 0.90 | 0.98 / 0.98 / 0.99 / 0.91 | 1.00 / 0.99 / 1.03 / 0.98 | 1.00 / 0.99 / 1.02 / 0.96 |
| dancers: frame / sunlit sand / concrete / rock | 0.98 / 0.98 / 0.95 / 1.04 | 0.98 / 0.99 / 0.95 / 1.04 | 1.01 / 0.99 / 0.99 / 1.06 | 1.00 / 0.99 / 0.98 / 1.06 |
| suit: frame / sunlit sand / white suit / red barrel | 0.98 / 0.94 / 0.95 / 0.88 | | 1.01 / 0.96 / 1.00 / 0.94 | 1.00 / 0.96 / 0.98 / 0.92 |

Sunlit regions move by 0-2 %, the hall is as without reflections, the shaded bench and wall reach 0.88 and 0.82 of path tracing.
The shaded sand next to the building overshoots (1.09 -> 1.29): its grazing reflection sees open sky in the probes (1 m above
the ground, L1), where path tracing's rough lobe also meets the grass, the clutter and the shadowed ground.

Sheets (the UE display at the interactive settings, auto exposure; before | after | path traced):
`<bench root>\look\25_reflect_{picnic,dancers,path,shade,hall,suit}.png`, `25_reflect_path_crop.png` (the bench and wall at full
size). Runs: `<bench root>\out\garden_5d` (`b_` before, `a_` AO 1, `v0_r_`/`v0_u_` floor 0, `x2_` visibility squared, `x3_` final).

## Checks

- `tests_reflections.cpp`: D3D's cube face layout (every texel's direction selects its own face and coordinates), the mip /
  roughness mapping, a constant sky prefilters to itself at every roughness, a half sky (zenith darkens with roughness, the
  horizon stays half), the DFG table against a midpoint integration of `Bsdf()`'s own lobe (largest difference 0.007 at 4096
  samples), its bounds (A, B >= 0, A + B <= 1, head-on smooth = f0), specular occlusion, the reflection visibility floor, the
  probes' visibility (open, closed, beside a wall, the interpolation, the side test, no valid corner), the option and settings.
- Identity: runs without reflections write the frames of the build before them: the lab without `--hdri` (plain, and with
  `--csm --ao clean --ao-strength 1.5 --probes --lamp-tubes --lamp-shadows --fxaa`) and the garden path view with
  `--sky-reflections 0` (AO 1) are byte-identical; path tracing is byte-identical (`r_path_pt_fixed` vs task 5c's).
- Startup: the cube and the table in 50-76 ms; the probe trace with the visibility rays 36.6 ms as before (garden: 52.6 % of
  the rays escape on average, 9.8 % of the valid probes enclosed; lab 20.1 % / 59.6 %).

## Limits

- Only the sky is reflected: no screen-space or local reflections. Where path tracing reflects the sunlit ground or a lit wall
  (grazing views of glossy surfaces), raster stays darker; occluded directions reflect nothing.
- The probe visibility is L1: it cannot hold a narrow opening (a window, a gap in a canopy); the probe spacing (1 m) blurs it.
- The characters do not occlude the sky in the probes; the foliage occludes it at its start pose.
- One cube for the whole scene, at infinity (no parallax), as a game's sky light.
