# pw_bench12 raster mode: baked lighting and the outdoor yard

The baking workflow, rendering with the lightmap and the outdoor lab. The raster mode itself: [RASTER.md](RASTER.md); the
measurements: [RASTER_ACCEPTANCE.md](RASTER_ACCEPTANCE.md).

## Baked lighting

The lightmaps for the raster mode's baked lighting come from
`tools/bench/blender/bake_lab_lightmaps.py` in a background Blender. Run it from the repository root, and only when no
other bench or bake is using the GPU. The interactive Blender is not involved, and the `.blend` is not modified.

```bat
"C:\Program Files\Blender Foundation\Blender 5.2\blender.exe" --background --factory-startup --python-exit-code 1 ^
    tools\bench\assets\lab_scene.blend --python tools\bench\blender\bake_lab_lightmaps.py -- --out tools\bench\assets
```

`--python-exit-code 1` makes a failed script exit with code 1.

The script works in this order:
1. Lightmap UVs.
2. The export, and its check against `lab_scene.glb`. A stale `lab_scene.glb` fails here, before any bake.
3. The JSON.
4. A preflight: what the bakes hide and mute, tried and undone at once.
5. The three bakes, tile by tile.
6. The denoise.
7. The JSON again.

Options after `--`:

| Option | Default | Meaning |
| --- | --- | --- |
| `--samples` | `512` | Cycles samples per texel. |
| `--size` | `8192` | Atlas size in texels. |
| `--tile` | `2048` | Bake the atlas in tiles of this size (VRAM; `0` = the whole atlas at once). |
| `--margin` | `4` | Texels around each UV island, and of bake dilation. |
| `--bounces` | `4` | The bench's default `--bounces`. |
| `--sun-dir`, `--sun-strength`, `--sun-angle` | `0.45,-0.77,0.45`, `1500`, `0.5` | The standard lab command's sun. |
| `--sky` | `0.12,0.16,0.23` | The constant sky. |
| `--hdri` | | The World is this Radiance panorama instead of `--sky`: the bench's `--hdri` (see Garden). |
| `--scale-full`, `--scale-sun` | `1`, `1` | `scaleFull` and `scaleSun` written to the JSON. |
| `--prefix` | `lab_lightmap` | The file names of the maps and the JSON (see Outdoor yard). |
| `--glb-name` | `lab_scene_baked.glb` | The baked export's file name. |
| `--no-bake` | | Everything except the bakes and the denoise. |
| `--no-denoise` | | Keep the raw bakes as the final maps. |
| `--denoise-only` | | No bake: make the final maps again from the `*_noisy.hdr` files in `--out`, and set `denoised` in the JSON. Needs no `.blend`. |

**Outputs** in `tools/bench/assets/` (local files, listed in `.gitignore`):

- **`lab_scene_baked.glb`:** the lab export of `export_bench_scene.py`, plus `TEXCOORD_1` and mesh
  `extras.lightmapAtlas` (0) on every static mesh.
  - Same as `lab_scene.glb`: the nodes, in the same order, with the same triangles and materials.
  - Static meshes are single-user (630 meshes instead of 304), because each instance needs its own texels.
  - The 40 meshes without UVs get a `TEXCOORD_0` of (0, 0).
  - The lightmap seams add 13 % vertices: 238 MB instead of 203 MB.
- **`lab_lightmap_full_0.hdr`, `lab_lightmap_sun_0.hdr`, `lab_lightmap_lamp_0.hdr`, `lab_lightmap_rest_0.hdr`:** Radiance,
  8192², DIFFUSE without colour.
  - `full`: direct and indirect light of the lamps, screens, sun and sky. Kept for previews.
  - `sun`: direct sun only.
  - `lamp` (bakes from 2026-10-07 on): the lamp groups' direct light only (every emissive material is a lamp group in
    bench12: tubes, screens, signs, the yard's poles).
  - `rest`: `full − sun − lamp` (`full − sun` in older bakes), as a map of its own: the indirect light of everything and the
    sky's direct light. RGBE keeps about 8 mantissa bits of the brightest channel, so subtracting files would lose the
    indirect light under the skylight. The renderer uses `rest + lamp + sun × dynamic shadow`, or `rest + lamp × lamp
    visibility + sun × dynamic shadow` with `--lamp-shadows`.
  - `*_noisy.hdr`: the three bakes before the denoise, always kept.
- **`lab_lightmap.json`:**
  - the atlases, each with its `full`, `sun`, `rest` and `lamp` files (`lamp` absent in bakes from before 2026-10-07);
  - the sun: travel direction as the bench's `--sun-dir` (bench space, see below), strength, angular diameter;
  - the sky;
  - `scaleFull`, `scaleSun` (1 until calibrated);
  - samples;
  - `texelsPerMeter` (size / √(static area));
  - `bakeSeconds`;
  - `denoised`: false when the maps are the noisy bakes (`--no-bake`, `--no-denoise`, or a failed denoise).

**How it bakes:**

- **Static and dynamic meshes.** Static is every mesh without an armature modifier, an armature parent or
  animation: 607 objects (the four sign texts become meshes), 1.13 M triangles, 7388 m². The 23 skinned meshes of the
  nine characters are dynamic. They are hidden during the bake, so their shadows stay dynamic. Foliage and props (objects
  named `veg_*` and `prop_*`, the garden's) are neither: they get no Lightmap UVs and are hidden during every bake.
- **Lightmap UVs.** Lightmap Pack makes one box per face, and at 1.1 M faces its smallest box swallows the atlas.
  Instead, all static meshes are copied into one world-space mesh, which gets Smart UV Project and then Pack Islands.
  The UVs go back to the scene loop by loop.
  - 57 % of the atlas is filled, 72 texels/m once packed (95 texels/m at most).
  - No texel is shared between objects, and texels of different objects are at least 8 texels apart.
- **The bake.** It runs on the world-space copy, not per object.
  - **Settings:** OptiX, 512 samples, 4 bounces, no clamping, a 4-texel margin (extend). Simplify limits textures to
    2048 during the bakes.
  - **Isolation:** only the copy and the sun render in every bake. Every other object is hidden: the 23 character
    meshes (their shadows stay dynamic), the original meshes, any light, any instancer.
  - **The `sun` bake** sets every emission strength and the sky to 0 (109 node trees in the lab) and keeps the direct
    pass. It refuses to run if a strength is linked or animated, or if a node may emit with no strength to mute.
  - **The `lamp` bake** hides the sun and sets the sky's strength to 0 (the materials keep their emission) and keeps the
    direct pass: the lamps' direct light, with the static shadows of everything that renders.
  - **Tiles:** Cycles bakes one image in one session, and the whole 8192² atlas needs more VRAM than is free. So each
    map is baked in tiles of `--tile` texels:
    - for each tile, the Lightmap UVs are moved so that the tile fills a tile-sized image;
    - the pixels are copied into a float atlas, and the UVs are restored.
    - Every texel is still baked once; the result differs from an untiled bake only by noise.
    - The margin is filled per tile, so a gap texel at a tile border is filled from its own tile.
  - **Image colour space:** the float images the scripts create are Non-Color. A new float image is tagged sRGB, and
    saving it as `.hdr` converts the values (a 0.30 mean became 0.44).
- **Denoise.** OIDN through the compositor (Denoise node, HDR, no guide passes). `sun`, `lamp` and the rest of `full` are
  denoised separately:
  - `sun := D(sun_noisy)`, `lamp := D(lamp_noisy)`;
  - `rest := D(max(full_noisy − sun_noisy − lamp_noisy, 0))`, never negative;
  - `full := rest + sun + lamp`.

  A bake without `*_lamp_0_noisy.hdr` (from before 2026-10-07, with `--denoise-only`) is finished as before:
  `rest := D(max(full_noisy − sun_noisy, 0))`, `full := rest + sun`, and no `"lamp"` in the JSON. If the denoise fails, the
  final maps are the noisy ones (`rest` = `max(full − sun − lamp, 0)`, not denoised) and the JSON says `"denoised": false`.
  Then run `--denoise-only`.
- **Sun and sky.** They match `run_lab.bat`:
  - the sun lamp's −Z is the bench direction in Blender axes `(x, z, y)`: (0.450, 0.450, −0.771). `--sun-dir` is in
    the bench's left-handed space, which is glTF with z negated (`pw_gltf.h`); glTF `(x, y, z)` is Blender
    `(x, −z, y)`. The first bake (2026-10-05) used `(x, −z, y)` on the bench vector, which mirrored the sun in z: the
    skylight's sun landed on the floor in front of the back wall instead of on the back wall;
  - its angle is 0.5° (the bench's `--sun-angle` is a diameter too), its strength 1500 W/m²;
  - the World is the constant (0.12, 0.16, 0.23) of `scene.cpp`, the path tracer's sky without `--hdri`;
  - the haze (`--haze`) is not baked.
- **Checks.**
  - The script refuses static objects with an enabled modifier or an active shape key, because the copy is built from
    mesh data.
  - It compares the world-space copy with the scene: positions, normals, texture UVs, materials.
  - It compares the export with `lab_scene.glb`.
  - It rasterizes the lightmap UVs at the atlas size. One static object (3 cm², a bottle label) covers no texel centre,
    so it stays black.
  - `python tools\bench\blender\lab_lightmap_check.py <lab_scene_baked.glb> [lab_scene.glb] [--raster 8192]` repeats
    the export checks without Blender.

**Time** (RTX 4080 SUPER, OptiX, 512 samples, 2048² tiles): the `--no-bake` part takes 1-2 minutes (UV packing
3-6 s); the `full` bake 206 s, the `sun` bake 119 s, the denoise 21 s (CPU); about 8 minutes end to end. Density:
72 texels/m packed (95 at most), one 8192² atlas. That is below the spec's ~100; no blockiness was visible in the bench
frames, so there is no second atlas.

### Rendering with the lightmap (`--lightmap`)

```bat
pw_bench12.exe 120 --gltf ..\bench\assets\lab_scene_baked.glb --lightmap ..\bench\assets\lab_lightmap.json --render raster --upscaler none --render-scale 1 --camera snap --sun-dir 0.45,-0.77,0.45 --sun-strength 1500 --exposure 0.22 --haze 0.008 --fov 62 --width 3840 --height 2160 --dump 0,3,30
```

- **Static pixels** (triangles whose vertices have lightmap UVs, atlas ≥ 0): baked diffuse plus dynamic specular, as
  games do it:
  `albedo × (rest × scaleFull + sun × scaleSun × dynShadow) + emission + specular`.
  - `specular` is the dynamic raster's sun and lamp light with the diffuse albedo removed: the same BRDF (`Bsdf`, with
    roughness and metal from the material), the same lamp groups as point lights with the same power, and the run's
    sun.
    - The sun's specular is shadowed by the first shadow map (every caster), so static occluders shadow reflections
      too; the lamps' specular is unshadowed, as in the dynamic raster.
    - A lamp group is one point light at its centroid, so the glossy floor reflects a highlight per group, not the
      tube shapes path tracing reflects.
    - No diffuse sun, lamp or ambient light is added: it is in the bake.
    - The diffuse albedo is `base × (1 − metal)`, so a fully metal surface has albedo 0: it is emission plus specular,
      as in the dynamic raster.
  - `rest` is the atlas entry's `"rest"` map (the bake's denoised `full − sun`) when the JSON lists one. Otherwise it is
    `max(full − sun, 0)`, computed per pixel. `"rest"` is on every atlas or on none.
  - A JSON with `"lamp"` maps (every atlas or none, only with `"rest"`): without `--lamp-shadows` the loader sums `rest + lamp`
    in floats before the packing, so `rest` above is again the bake's `full − sun`; with it the lamp map is shaded apart
    (RASTER.md, "Lamp shadows"): `albedo × ((rest + lamp × lampVis) × scaleFull + sun × scaleSun × dynShadow)`.
  - The maps are sampled at the pixel's `TEXCOORD_1`: bilinear, clamped to the atlas edges.
  - `scaleFull` and `scaleSun` come from the JSON.
  - `dynShadow` is the 3x3 PCF of a second 2048² sun shadow map, which is drawn with the triangles without baked light
    alone: the dynamic ones (`[staticTriangles, triangleCount)`) and the static ones without lightmap UVs (the garden's
    foliage without `--wind`, and its props). It is 1 where no character or plant occludes the sun, so their sun shadows darken the
    baked sun on static surfaces. When the scene has static triangles without lightmap UVs the draw walks
    `Scene::unbakedTriangles` (`raster_shadow_list_vs.cso`, the baked root's parameter 10); otherwise (the lab, the yard) it
    is drawn as before.
- **Dynamic pixels** (the characters, and static triangles without lightmap UVs) are shaded as without `--lightmap`.
  This includes the first shadow map, which keeps every triangle, so the hall still shadows the characters.
- **Fog:** unchanged. Baked pixels also pass the first shadow map's sun visibility to the fog.
- **Without `--lightmap`** nothing new is created: no lightmap buffer (even for a glb with `TEXCOORD_1`), descriptor,
  root signature or PSO. The raster shaders are the same binaries; the baked mode uses
  `raster_lightmap_{vs,ps}.cso` and `raster_shadow_dynamic_vs.cso`, built from the same sources with `PW_LIGHTMAP=1`.
- **Loading:** each atlas loads two maps: `rest` (or `full`, or `rest + lamp`) and `sun`; with `--lamp-shadows` a third,
  `lamp`. Each goes to the GPU as `R9G9B9E5_SHAREDEXP`
  without mips: 268 MB per 8192² map, so one atlas takes 537 MB. The atlas files are relative to the JSON's folder.
- **Errors at startup:**
  - `--lightmap requires --render raster`;
  - `--lightmap needs a scene with lightmap UVs (TEXCOORD_1), e.g. lab_scene_baked.glb`;
  - `Cannot open lightmap <file>: <reason>`, for every problem with the lightmap files:
    - the JSON or an atlas file is missing;
    - the JSON is malformed or has a missing or mistyped field (`lightmap JSON: ...`);
    - the scene's UVs address more atlases than the JSON lists, checked before any atlas is read;
    - an atlas is not Radiance, or its size differs from the JSON's.
- **Warnings, at most once per run each:**
  - `lightmap baked for sun ...; this run uses ...`, when the run's sun (direction, strength or angle) differs from the
    baked one by more than 1e-3;
  - `lamp groups changed (...)`, when a lamp group's intensity or tint differs from the bake (1, white), from
    `settings.json` or the menu. The baked light keeps the bake's lamps; the emission and the characters' lamp lights
    follow.

### Scales

The scales live in the local `lab_lightmap.json`; a fresh bake writes `--scale-full` and `--scale-sun` (default 1).
Set them after every bake, or pass them to the bake:

```json
"scaleFull": 1.3, "scaleSun": 1.0
```

How they were found, and what they give against path tracing: [RASTER_ACCEPTANCE.md](RASTER_ACCEPTANCE.md).

### The lamp-split bakes (2026-10-07)

The bakes with the `lamp` map, for `--lamp-shadows` (RASTER.md, "Lamp shadows"), are under their own names, so the bakes
above keep their files (and an older exe reading them keeps its light):

| Scene | `--prefix` | `--glb-name` | Time | `full` / `sun` / `lamp` bake | Means: full, sun, lamp, rest |
| --- | --- | --- | --- | --- | --- |
| lab | `lab_lightmap_lampsplit` | `lab_scene_baked_lampsplit.glb` | 11.5 min | 231 / 129 / 178 s | 30.40, 30.00, 0.107, 0.293 |
| yard | `lab_outdoor_lightmap_lampsplit` | `lab_scene_outdoor_baked_lampsplit.glb` | 12.6 min | 286 / 125 / 192 s | 63.02, 61.57, 0.073, 1.378 |
| garden v2.1 | `lab_garden_lightmap` | `lab_scene_garden_baked.glb` | 12.6 min | 249 / 133 / 187 s | 67.92, 53.85, 0.064, 14.01 |

All with `--scale-full 1.3 --scale-sun 1.0` (RTX 4080 SUPER, OptiX, 512 samples, 2048² tiles; denoise 28-36 s). The garden
keeps its own names (its files belong to the garden build and are rebaked with it); its bakes have `--hdri`.
In the lab the lamps' direct light is about a quarter of the non-sun baked light.

## Outdoor yard (`lab_scene_outdoor.glb`)

Inside the hall the standard sun reaches only the back wall through the skylight, so cascades and baked sun shadows
have little to show. The outdoor lab adds a sunlit yard where the combo camera leaves the hall.

**Build** (background Blender, about 2 minutes; `lab_scene.blend` and `lab_scene.glb` are read, never written):

```bat
"C:\Program Files\Blender Foundation\Blender 5.2\blender.exe" --background --factory-startup --python-exit-code 1 ^
    tools\bench\assets\lab_scene.blend --python tools\bench\blender\build_lab_outdoor.py -- --out tools\bench\assets
```

- `build_lab_outdoor.py` opens the lab, cuts the opening, adds the yard (`lab_outdoor_geometry.py`), saves
  `lab_scene_outdoor.blend` and exports `lab_scene_outdoor.glb`. The export is `export_bench_scene.py`'s: the nine
  characters of `lab_characters.blend`, the tinted base colours composed.
- **Checks** against `lab_scene.glb`:
  - the lab's 1310 nodes come first, in the same order;
  - they have the same triangles and materials, except `shell_walls`, which has the opening (36 triangles become 68);
  - the 12 yard nodes come after them;
  - the 9 skins and the animation's 1737 channels are unchanged, sampled values included.
- `lab_scene_outdoor.opening.json` records the opening, the three wall faces removed, and the yard's objects.

**The opening.** The combo camera's eye crosses the hall's east wall (x 9.65-9.95, Blender axes) at frame 882.8,
at y 6.39 and 1.9 m high. The opening is 8 m × 5 m: y 2.39-10.39, from the floor to 5 m. The wall's inner face,
outer face and bottom are bisected there, the three faces inside are removed, and two jambs and a lintel close the
wall's thickness. Inside, the gallery (3.6-3.9 m) still crosses the top of the opening.

**The yard** (collection `Yard`, nodes `yard_*`), 40 m deep (x 9.8-49.8) and 60 m long (y −23.6 to 36.4):
- the ground: one quad with the lab's concrete texture, tiled every 4 m;
- 4 buildings, 15, 12, 10 and 8 m high, boxes with concrete or corrugated-iron walls and a dark roof, no bottom face;
- 3 crate stacks of 1.2 m crates;
- a 2 m fence on the three outer edges: posts every 2.5 m and two rails;
- 3 light poles, 8 m high, each with an emissive head. Each head has its own material (`yard_lamp_a`, `_b`, `_c`,
  radiance 60), so each pole is its own lamp group in raster.

The sky stays open. At the standard sun (`--sun-dir 0.45,-0.77,0.45`, about 50° high) the sun comes over the hall:
- the hall's shadow covers the first ~5 m of the yard, where the camera is at frame 900;
- the buildings, poles, fence and crates cast shadows towards +x/+y, 0.84 times their height.

**Exposure.** The lab's `--exposure 0.22` blows the sunlit yard out to white. Path-traced previews of the yard use
`--exposure 0.025`. The shade is then nearly black, since the constant sky (0.12, 0.16, 0.23) is about 1/100 of the
sun.

**Bake** (the same script, outdoor names; GPU only when no bench runs):

```bat
"C:\Program Files\Blender Foundation\Blender 5.2\blender.exe" --background --factory-startup --python-exit-code 1 ^
    tools\bench\assets\lab_scene_outdoor.blend --python tools\bench\blender\bake_lab_lightmaps.py -- ^
    --out tools\bench\assets --prefix lab_outdoor_lightmap --glb-name lab_scene_outdoor_baked.glb
```

- `--prefix` names the maps (`<prefix>_{full,sun,rest}_0.hdr`, the `*_noisy.hdr` files) and the JSON
  (`<prefix>.json`). `--glb-name` names the baked export. The defaults are the lab's names. The reference export is
  the `.blend`'s `.glb`: `lab_scene_outdoor.glb` here.
- **Outputs:** `lab_scene_outdoor_baked.glb`, `lab_outdoor_lightmap_{full,sun,rest}_0.hdr` and
  `lab_outdoor_lightmap.json`.
- **Static meshes:** 619 meshes, 1.13 M triangles, 12385 m² (the lab has 7388 m²).
- **Density:** one 8192² atlas, 58 % filled. That is 56 texels/m once packed (74 at most), against the lab's 72.
  The script has no second atlas. Six tiny static objects (19 cm² in all, bottle labels) cover no texel centre and
  stay black (the lab has one).
- **Time** (2026-10-06, RTX 4080 SUPER, OptiX, 512 samples, 2048² tiles):
  - 8.5 minutes end to end;
  - the `full` bake 225 s, the `sun` bake 130 s, the denoise 20 s.
- **Scales:** the JSON gets the lab's calibration, `"scaleFull": 1.3, "scaleSun": 1.0`, by hand after the bake. It
  has not been calibrated against path tracing outdoors yet.

## Garden (`lab_scene_garden.glb`)

The outdoor lab with a garden in the yard: a path along the combo camera's route, card grass, trees, ferns and props
(benches, tables, rocks, pots, barrels, yard tools). It is
the scene for foliage, wind (`--wind`) and the HDRI sky (`--hdri`, `--raster-sky-ambient`).

**Inputs** (local, untracked): `lab_scene_outdoor.blend`; the Poly Haven glTFs `polyhaven/island_tree_02/` and
`polyhaven/fern_02/`, the props' glTFs (2k; see Props below) and the textures `polyhaven/grass_ground/` and `polyhaven/grass_path_2/` (2k JPEG diff, nor_gl,
disp; CC0, read only); for the bake, `autumn_field_puresky_nosun.hdr` from `tools/bench/hdri_prepare.py`.

**Build** (background Blender, about 3 minutes; `lab_scene_outdoor.*` are read, never written):

```bat
"C:\Program Files\Blender Foundation\Blender 5.2\blender.exe" --background --factory-startup --python-exit-code 1 ^
    tools\bench\assets\lab_scene_outdoor.blend --python tools\bench\blender\build_lab_garden.py -- --out tools\bench\assets
```

- `build_lab_garden.py` adds the garden, saves `lab_scene_garden.blend` and exports `lab_scene_garden.glb`
  (`export_bench_scene.py`'s export, characters appended). Modules: `garden_path.py` (route and path strip),
  `garden_soil.py` (the soil bed),
  `garden_ground.py` (the soil and path textures), `garden_layout.py` (positions), `garden_grass_atlas.py` (the blade
  atlas), `garden_grass.py` (clumps), `garden_plants.py` (trees and ferns), `garden_props.py` (the props' templates
  and instances), `garden_props_layout.py` (the hand-placed positions and the checks), `garden_props_scatter.py` (the
  scattered clusters), `garden_props_ground.py` (each prop on the surface under its feet) and
  `garden_props_assembly.py` (the bench kit assembled, every template's parts checked).
- **Node order.** The glTF exporter writes root objects sorted by name, so the garden hangs under an empty
  `zz_garden` at the origin: its nodes (183 in v2.3) come after the outdoor lab's 1322.
- **Checks** against `lab_scene_outdoor.glb`: the 1322 nodes first, in the same order, none changed; the garden's
  nodes after them; the 9 skins and the 1737 animation channels unchanged. The foliage materials are alphaMode
  `MASK` (cutoff 0.5) and double-sided, and no foliage primitive has `TEXCOORD_1`. The props: 120-170 nodes sharing
  one mesh per template used, at most 1.4 M triangles flattened, no `TEXCOORD_1`, no emission (the bench would make a
  lamp of it), the potted plant's leaves `MASK`.
- `lab_scene_garden.layout.json` records the route, the trees and ferns, the counts and the checks.

**The garden** (Blender axes; collection `Garden`):
- **Route:** the combo eye from the hall's opening (frame 884, x 9.95) to where it leaves the yard through the south
  fence (frame 1121): 34.7 m.
- **`garden_soil`** (`garden_soil.py`, garden v2.1): a flat bed 1 cm above the ground under all the grass: a strip
  out to about 8.5 m from the route (its edge wandering ±0.8 m) and every zone where the layout may put a clump (the
  band, the fence, wall and tree patches and their stragglers) widened by 0.4 m, so no clump stands on concrete. Its
  outline is the 0.5 level of a field on a 0.25 m grid, blurred over 1.2 m with fine noise only inside that
  transition, traced by marching squares: a ragged edge of soil tongues and bays onto the concrete instead of the
  strip's straight line (v2). 34946 triangles, 14 pieces (islands under 0.08 m² dropped), 1034 m².
  **`garden_path`:** a strip 2 cm above the ground, 3 m wide: the 2.5 m path and 0.25 m on each side where it blends
  into the ground, cut to the yard inside the fence. Both are static and baked (not `veg_`).
- **Ground textures** (`garden_ground.py`). The bench reads one base colour and one normal map per material, so the
  layers are composed in numpy and written as JPEG next to the scene (then packed into it):
  - `garden_ground_{diff,nor}.jpg`, 4096², the soil: one 5 m world-space tile (UV = world xy / 5 m, 819 px/m) made
    of three copies of `grass_ground` (2.5 m tile; as is, turned 90° and turned 270°, offset), each where a periodic
    noise plus its height map is highest (a height blend with narrow transitions). It repeats every 5 m instead of
    every 2.5 m.
  - `garden_path_{diff,nor}.jpg`, 2048 × 4096, the path: 3 m across (u) by 5 m along the route's arc length (v).
    Two copies of `grass_path_2` (1 m tile, 683-819 px/m) under the same kind of mask; past a ragged line about
    1.1-1.35 m from the centre (noise and a height blend) two copies of `grass_ground` take over, alone from 1.42 m.
    The soil's world-space ground meets the path mesh's strip-space ground at 1.5 m, under the first grass.
  - Turned normal maps turn their vectors too; the build checks every turn against the height map's slopes.
  - Roughness is constant (0.95 soil, 0.9 path); the maps' roughness is near 1 everywhere. About 255 MB of RGBA8
    textures with mips in the bench.
- **Grass, `veg_grass_NN`** (garden v2.1, 2026-10-07; v2.2 removes the 542 clumps by the props): 4872 clumps in 13 nodes (≤ 500 each, by 8 m cells), 21904
  quads, 43808 triangles (the build refuses more than 60000; v2 had 2387 clumps, 21504 triangles).
  - Clump: 4-5 quads at varied angles (k·180°/n, each jittered by up to 40 % of the step), 0.3-0.6 m tall and
    0.5-0.9 m wide (about 1.5 × the height). Each quad stands 2-9 cm off the centre and leans 6-18° outward; its
    bottom vertices are on the ground, so bench12's wind finds each quad's base and top. Each quad shows a random,
    randomly mirrored atlas cell. Near the path a clump is narrowed to reach at most 0.15 m over the edge.
  - Atlas: `garden_grass_atlas.png`, 4096 × 2048, 4 × 3 cells of 1024 × 683 (the clumps' 1.5 aspect). 12 clusters of
    28-46 blades each: tall blades fanning out from a narrow base (some arching over with a drooping tip) and short ones
    at the base, three vertices across (a lighter midrib), varied greens, about a third with yellowed tips and a few dry
    all along; 7 clusters carry 1-3 seed stems with a head of spikelets. Rendered by Cycles (emission, transparent
    film); the colour of transparent texels is filled from their neighbours.
  - Four materials `veg_grass_0..3`: the atlas times a slight tint, alpha through Math Round (`MASK`, 0.5).
  - Placement: a seeded Poisson process thinned by a density map, with clumps at least 0.14 m apart (v2: 0.2 m,
    which capped the density near 17/m²):
    - no clump centre on the path or within 0.3 m of its edge; a ramp to full density by 1.6 m whose start wanders
      up to 0.9 m outward (a ragged line of thinning grass);
    - 26 clumps/m² out to 3.5 m, fading out by 5.5 m; the band's outer edge wanders by noise (±1.3 m coarse, ±0.35 m
      fine);
    - patches of 14/m² along the fence, the building walls and the hall's east wall, and under the trees, with the
      same ragged edges;
    - stragglers (0.8/m² in small groups) out to 2 m past the band's and the patches' edges and in the thinning
      ramp; none past 8.2 m from the route except by the patches.
- **Trees, `veg_tree_0..3`:** `island_tree_02`, one realised mesh each, base scale 1.4 (the asset is a 3.4 m tree),
  rotation and scale varied (±12 %), sunk 5 cm (the trunk has a flat bottom cap).
  - Decimated per material (collapse) from 1.07 M to 146999 triangles: trunk `island_tree_02` 13 k, leaves 100 k,
    branches 34 k.
  - The trunk material keeps Poly Haven's name, `island_tree_02`, which bench12's wind holds still.
  - The branches' `KHR_texture_transform` is baked into their UVs, because the bench reads no texture transform.
  - Trees 0 and 1 stand 4.6 m and 5.0 m outward of the route at combo frames 975 and 1045, where the camera looks.
    Trees 2 and 3 stand 11.5 m and 10 m out at frames 1003 and 925.
- **Ferns, `veg_fern_00..14`:** the four `fern_02` variants (24 k triangles in all), scale 1.1-1.4. Two stand by each
  tree, three along building 1's west wall, two along building 3's, and two along the hall's east wall.
- **Alpha.** The downloaded textures are JPEGs, so the glTFs' alpha is lost. The scripts rebuild it and pack an RGBA
  PNG per material:
  - the leaves' alpha from their diffuse map (black around each leaf);
  - the fern's from its ARM map (roughness 1 around each frond).
- **Props, `prop_*`** (garden v2.2, 2026-10-07; v2.3 the same day: `garden_props.py`, `garden_props_layout.py`,
  `garden_props_scatter.py`, `garden_props_ground.py`): 151 objects (v2.2: 31) of the 27 templates, 1014376 triangles
  as bench12 draws them (one copy per node), 255646 in the 27 shared meshes (the build refuses fewer than 120, more
  than 170 or more than 1.4 M), from 18 Poly Haven glTFs the owner chose (`polyhaven/<id>/<id>_2k.gltf`, CC0, read only).
  - **Shared meshes (v2.3).** Each prop is an object of its template's one mesh (location, turn about z, uniform
    scale); the glTF export keeps that: 27 meshes, 151 nodes with their TRS. `lab_scene_garden.glb` 322.8 MB (v2.2
    324.4 MB with 31 realised props). bench12's loader (`pw_gltf_vertices.h BuildVerticesSubset`) still flattens: each
    node's primitives are expanded (non-indexed, 3 vertices a triangle) into the one vertex buffer and the one static
    BLAS, so a shared mesh costs as much as copies; instancing it is a later engine step.
  - **Templates** (27): some of an asset's nodes joined into one mesh, centred on its footprint, front -y, standing on
    its lowest vertex or (rocks, boulder) on the asset's own ground, sunk where it should be (set 2 rocks 12 cm, the
    boulder 15 cm; set 1's rocks are modelled sunk). Decimated (collapse): boulder 66 k → 26.6 k, dead trunk 102 k →
    20 k, the potted plant's soil 54 k → 2 k (the plant 17.9 k), each rock 5 k (set 1 three rocks 15 k, set 2 five
    25 k). The modular bench: one section (legs, seat with back, arm rests; 12.5 k) and a double one with the section
    mirrored about the shared double leg (21.2 k). The seating glTF is a kit laid out exploded (parts about 13.7 cm
    apart along the bench and 10 cm in height); v2.2 joined it as authored, so the benches came apart (slats off the
    legs, a leg standing apart). v2.3 assembles the section (`garden_props_assembly.SEATING`: seat, back and crossbar
    towards the double leg, the single leg after them, the arm rests down onto the legs' tops, the support and the
    crossbar up under the seat): 2.18 m (double 4.17 m) instead of 2.45 (4.72). Every template's parts are checked:
    parts whose boxes touch (within 1 cm) form groups and each group must reach the template's base (the crates are
    two groups, each on the ground); the template's lowest point must be its base. One UV map each (the seating's
    unused second set removed).
  - **Scatter (v2.3, `garden_props_scatter.CLUSTERS`, seed 23):** 120 more: a double bench and a bench facing the
    path, 36 rocks and dry branches 1.9-3.2 m from the route on both sides and 22 (with a boulder and a trunk now and
    then) 3.2-6.5 m out, 4 under each tree, 8 yard things (barrels, buckets, jerrycans, cement and compost bags, pots,
    crates, a hand truck) on each side of the hall's opening, 8 along the hall's wall south of it, 5 by building 3, 6 by
    building 1, 9 by the two crate stacks. Random turns; rocks of set 2 at 0.45-0.9, set 1 at 0.35-0.55, branches
    0.85-1.15 of their size. A candidate is kept only if `Site.problems` (the checks below, plus the fence and 12 cm
    between footprints) finds nothing against every prop placed before it.
  - **On the ground (v2.3, `garden_props_ground.py`, a build check):** a template's supports are its vertices within
    1 cm of its lowest point (at most 96, one per 2 cm cell); a ray down under each finds the yard's concrete, the soil
    bed (1 cm up) or the path (2 cm up), or the prop below for a stacked one. A rigid prop rests on the support that
    needs the most height, 5 mm into the surface; a buried one (rocks, boulder, trunk, branches) keeps its designed
    depth below the lowest ground. The build prints every prop's gaps and fails if a support floats more than 2 cm
    (for a stacked prop: the prop as a whole) or sinks more than 8 cm: v2.3's largest gap on the ground is 1.4 cm (the
    crates straddling soil and concrete), the deepest -1.1 cm. v2.2 stood every prop at z = 0 by its template, so
    those on the soil sank 1 cm; none floated as a whole (measured on the v2.2 .blend and glb): the "floating
    bench" the owner saw was the seating kit's exploded parts (see Templates).
  - **Placement** (the 31 hand-placed ones in `garden_props_layout.PROPS`): a double bench beside the path on its outer side
    (combo frame 1015) and a single one on the inner side (1052), both facing it; the picnic table under tree 0 and the
    table with two chairs on the inner side; eight rocks (0.66-1.0 of their size), the boulder, the dead trunk and three
    dry branches in the grass beside the path and under the trees; two pots, three barrels, the jerrycan and the bucket
    by the hall's east wall south of the opening; the hand truck, the compost bags, three cement bags (one on the two
    others), the two military crates and the gloves (on a crate's lid) by building 3's west face.
  - **Checks** (on the real footprints, the build raises): nothing within 1.75 m of the route (the path mesh's 1.5 m
    half width plus 0.25 m; the closest is the double bench at 1.89 m), the combo eye (every frame 0-1399) never within
    0.5 m of a footprint, nothing in a building, crate, pole or the hall's wall, at least 0.6 m from a trunk and
    0.75 m from a fern, no two props overlapping (except the stacked ones), the rocks, boulder, trunk and branches on
    the soil bed. Grass clumps within 0.35 m of a prop's footprint (its vertices below 0.7 m) are removed: v2.3 1675
    of 4872, leaving 3197 clumps, 28768 triangles (v2.2: 542, 4330 left); the rest of the grass is v2.1's.
  - **Lit dynamically.** Not baked: `lab_lightmap_uv.is_unbaked` (`veg_`, `prop_`) gives them no Lightmap UVs and
    hides them in every bake, `lab_lightmap_check` refuses `TEXCOORD_1` on them. bench12's dynamic-only sun cascades and
    lamp maps draw `Scene::unbakedTriangles` (every triangle whose vertices have atlas -1: dynamic ones and static ones
    without `TEXCOORD_1`, `lightmap_uv.h`), so the props cast the sun's and the lamps' shadows on the baked ground. Not
    `veg_`, so `--wind` leaves them alone. The lab already has 93 `prop_*` nodes, all of them empties (parents of
    meshes with other names); the build checks that the only `prop_` mesh nodes are the garden's.
  - **Materials:** Poly Haven's (base colour, normal, ARM) with every image scaled to at most 1024 and written as JPEG
    to `garden_props/` (72 images, ~410 MB RGBA8 with mips in the bench instead of ~1.6 GB at 2k); the potted plant's
    leaves cut out from their diffuse map's black background (`MASK`); the compost bags' transmission dropped.

**Exposure** (garden v2.1, 2026-10-07): the garden's standard exposure is **`--exposure 0.007`** (with
`--hdri autumn_field_puresky_nosun.hdr`, the lab's sun 1500 and haze 0.008, ACES). At 0.02-0.025 (the yard's) the
sunlit path and soil sit on the tonemap's shoulder and lose their texture. Measured on path-traced RR dumps, ground
pixels found by casting each pixel's ray to the ground (`<bench root>\scripts\garden_exposure.py`), grass left out:

| Dump | Exposure | Sunlit path p50 / p99 | Sunlit soil p50 / p99 | Shaded soil p50 | Grass p50 |
| --- | --- | --- | --- | --- | --- |
| combo 990 (the path is out of view) | 0.010 | - | 205.5 / 235.4 | 82.8 | 114.0 |
| combo 990 | **0.007** | - | 186.7 / 226.0 | 75.0 | 94.4 |
| close static view (path in front) | 0.010 | 231.8 / 246.1 | 204.2 / 234.5 | 87.3 | 97.9 |
| close static view | **0.007** | 220.3 / 240.9 | 185.9 / 224.3 | 77.8 | 84.7 |
| close static view | 0.005 | 206.0 / 233.7 | 165.2 / 212.1 | 64.9 | 79.3 |

(sRGB luma 0-255; close view `--cam-pos 19.0,0.8,-4.0 --cam-target 20.2,0.1,-6.6`.) 0.007 keeps the sunlit soil's
99th percentile under 235 at combo 990 and the shade readable; the path's fine detail (high-pass standard deviation
in a sunlit crop) is 9.9 at 0.007 against 7.6 at 0.010. The path seen close up, facing the sun, still reaches a 99th
percentile of 241; 0.005 would keep it under 235 at the cost of darker shade.

**Bake** (GPU, only when no bench runs):

```bat
"C:\Program Files\Blender Foundation\Blender 5.2\blender.exe" --background --factory-startup --python-exit-code 1 ^
    tools\bench\assets\lab_scene_garden.blend --python tools\bench\blender\bake_lab_lightmaps.py -- ^
    --out tools\bench\assets --prefix lab_garden_lightmap --glb-name lab_scene_garden_baked.glb ^
    --hdri tools\bench\assets\autumn_field_puresky_nosun.hdr --scale-full 1.3 --scale-sun 1.0
```

- **The HDRI World** is the panorama at bench12's lookup (`Sky()` in `scene.hlsli`).
  - Blender's equirectangular lookup is the mirror image of the bench's (+X at u 0.5 in both, but +Y at u 0.25 in
    Blender and 0.75 in the bench), so a Mapping node negates the direction's y.
  - The rotation is 0, because the prepared file is already aligned to the bench sun, and the strength is 1, because
    the bench scales nothing.
  - Checked with a coded panorama: Cycles' background at seven directions matches the bench's (u, v) within one
    texel.
  - The image is read as Linear Rec.709.
  - The `sun` bake mutes the World's strength as it did the constant sky.
- **Foliage and props** get no lightmap: the `veg_` and `prop_` objects (`lab_lightmap_uv.is_unbaked`) are left out of
  the UVs and hidden in every bake, so the maps hold none of their shadows. Raster lights them and shadows them
  dynamically.
- **The JSON** has `"sky": null` and `"hdri": "autumn_field_puresky_nosun.hdr"`, and the scales 1.3 / 1.0 (the
  lab's, not calibrated outdoors).
- **Outputs:** `lab_scene_garden_baked.glb`, `lab_garden_lightmap_{full,sun,lamp,rest}_0.hdr` and the `*_noisy` files
  of the three bakes, and `lab_garden_lightmap.json` (with `"lamp"`, so `--lamp-shadows` works on the garden).
- **Static meshes:** 621 (the outdoor lab's 619 plus the soil and the path), 1.17 M triangles, 13522 m² (v2.1; the
  ragged soil bed adds about 500 m² and 35 k triangles over v2's strip).
- **Density:** one 8192² atlas, 55 % filled: 52.3 texels/m once packed (70.4 at most), against the yard's 56. Seven
  tiny objects (68 cm² in all, bottle labels, cables and a toggle) cover no texel centre.
- **Time** (garden v2.1, 2026-10-07, RTX 4080 SUPER, OptiX, 512 samples, 2048² tiles): 12.6 minutes end to end;
  the `full` bake 249 s, the `sun` bake 133 s, the `lamp` bake 187 s, the denoise 35 s. Means: full 67.92, sun 53.85,
  lamp 0.064, rest 14.01.
  - The garden's first bakes (v1, without `lamp`, 3568 and 4892 clumps) gave the same denoised full and sun means to
    four digits, as expected with no foliage in the bake; v2 changed only the ground's textures (and the path mesh's
    width): full 71.33, sun 56.68, lamp 0.067, rest 14.58 in 12.4 minutes. v2.1's means are about 5 % lower because
    its soil bed reaches into the shade of the buildings and the fence (more shaded texels), not because of the grass.
- **Garden v2.3 is not baked again either:** the same `--no-bake` export and comparison (621 static nodes, 1168497
  triangles, `TEXCOORD_1` SHA fb270b2a5e48049f both, positions, node order, TRS and atlas the same); only
  `lab_scene_garden_baked.glb` (358.0 MB) was replaced.
- **Garden v2.2 is not baked again.** The props change no static mesh, so `bake_lab_lightmaps.py --no-bake` (to a
  scratch `--out`) made the new `lab_scene_garden_baked.glb` and its 621 static nodes' `TEXCOORD_1` (1.17 M triangles,
  per corner, after the indices) were compared with v2.1's baked glb: bit-identical, as are the node order, the
  positions and `lightmapAtlas`. So v2.1's maps and `lab_garden_lightmap.json` stay; only the glb was replaced.
- **Raster note.** Foliage that is not dynamic (no `--wind`) has no lightmap UVs, so the baked mode's dynamic-only sun map
  (and, with `--lamp-shadows`, the lamp groups' dynamic-only maps) draws it with the dynamic triangles: the grass and the
  trees shadow the baked sun on the ground (since 2026-10-07; before, only the sun's specular).
