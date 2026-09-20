# pw_bench12: D3D12 path-traced bench (spec for the first milestone)

A standalone Windows program, `tools/bench12/`, next to the D3D11 bench in `tools/bench/` (do not modify
`tools/bench/`). It renders the glTF lab scene with hardware ray tracing (DXR 1.1 inline ray queries in a
compute shader) the way a path-traced game hands frames to DLSS Ray Reconstruction: a noisy 1-spp linear
HDR colour plus guide buffers. This milestone has NO NGX/DLSS yet: it renders, presents and dumps frames.

## Reuse
- `../bench/pw_gltf.h` (include it, do not copy): `pwgltf::Load`, `pwgltf::BuildVertices(scene, t, dt, verts,
  prevPositions, &ranges, &lights)` returns a flat non-indexed triangle list in bench space (left-handed,
  Y up), `Vertex{pos, normal, material, localY, stripe, colour[3], uv[2], texture, alphaMask, alphaCutoff}`.
  `material`: -1 ground, -2 plain, -4 emissive (colour = radiance), -5 glossy floor (roughness < 0.3).
  `scene.images[i]` holds encoded PNG/JPEG bytes: decode with WIC to RGBA8 sRGB (see `tools/bench/pw_bench.cpp`
  around line 1214 for the pattern), full mip chain is optional (mip 0 only is fine for now).
- Camera script and options: port `CameraAt` (pw_bench.cpp ~line 689), `--camera static|yaw|forward|strafe|combo|script`,
  `--cam-dolly`, `--cam-lift`, `--cam-pos x,y,z --cam-target x,y,z`, `--fov`, `--yaw-speed`, `--sway`, and the
  "scripted camera follows the character" offset (pw_bench.cpp, search "scripted camera follows").
- Frame dump: `--dump N[,N...]` writes `dump_<N>.bmp` of the presented image (pw_bench.cpp has a BMP writer).

## Files (keep each under ~300 lines, no logic in main beyond wiring)
- `main.cpp`       options, frame loop, wiring.
- `options.h`      struct Options + parser.
- `device.h/.cpp`  D3D12 device (feature level 12_1, requires `D3D12_RAYTRACING_TIER_1_1`, fail with a clear message
                   otherwise), direct queue, flip-model swap chain (RGBA8, the window is `--width/--height`, default 1920x1080),
                   fence/frame sync, a CBV/SRV/UAV descriptor heap allocator, upload helper, texture readback for the dump.
- `scene.h/.cpp`   loads the glb, builds GPU buffers: `StructuredBuffer<TriVertex>` (pos, prevPos, normal, uv),
                   `StructuredBuffer<TriMaterial>` per triangle (albedo rgb, roughness, emission rgb, texture index, alpha cutoff, flags),
                   textures (bindless array), emissive triangle list for light sampling (triangle index + area CDF + power).
- `accel.h/.cpp`   one BLAS over the whole triangle list (opaque flag per geometry: put alpha-masked triangles in a second
                   geometry without `D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE`), one TLAS with a single instance. Static for now.
- `pathtrace.h/.cpp` root signature + compute PSO, dispatch, the output textures.
- `camera.h`       camera script.
- `shaders/pathtrace.hlsl` compiled offline with dxc (`-T cs_6_5`), loaded from `shaders/bin/pathtrace.cso` next to the exe.
- `shaders/present.hlsl`  tone-map (or debug view) blit to the back buffer.
- `build.cmd`      vcvars64 + dxc (`C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\dxc.exe`) + cl. No CMake.
- `README.md`      purpose, requirements, build, run, options.

## The frame (render size = `--render-scale` x output size, default 0.5 like DLSS Performance; all guides at render size)
Primary rays from the jittered camera (Halton 2,3 sub-pixel jitter, `--jitter 0|1`, same convention as the D3D11 bench:
the jitter offset in pixels is reported later to NGX). For each pixel:
1. `RayQuery` primary hit (alpha-mask handled in the candidate loop by sampling the base colour alpha). Miss: sky from
   the HDRI (`--hdri file.hdr`, port `LoadHdr`) or a constant.
2. Outputs at the primary hit:
   - `depth`  R32F, standard 0..1 depth of the UNJITTERED projection by default (`--depth standard|reverse`), far = 1 on miss.
   - `motion` RG16F, pixels at render size, current -> previous (NGX convention), from the unjittered current and previous
     view-projection and the vertex prevPos interpolated with the barycentrics (static scene: prevPos = pos).
   - `normalRoughness` RGBA16F (world normal xyz, roughness w), `diffuseAlbedo` RGBA16F, `specularAlbedo` RGBA16F
     (F0 0.04 dielectric; glossy floor keeps 0.04 with low roughness), `specHitDistance` R16F (0 for now unless trivial).
   - `colour` RGBA16F linear HDR radiance, NOISY, 1 sample per pixel:
     emission at the hit
     + direct sun: one shadow ray towards a jittered direction inside the sun disk (`--sun-dir x,y,z` is the direction the
       light TRAVELS, `--sun-strength E`, `--sun-angle deg` default 0.5)
     + direct lamps: pick ONE emissive triangle by power CDF, uniform point on it, shadow ray, proper pdf
     + one indirect bounce: cosine-weighted diffuse direction (or a GGX-ish mirror-ward direction with probability
       ~F for the glossy floor), trace, shade that hit with emission + the same two direct terms (no further bounce).
     Per-frame decorrelated RNG (pcg hash of pixel and frame index). `--spp N` (default 1), `--bounces 0|1`.
   - `--exposure`, `--albedo` multipliers like the D3D11 bench. Clamp fireflies with `--firefly L` (default 50).
3. Present: `--view colour|albedo|normal|depth|motion|accum`. `accum` is a running average over frames of a static camera
   (reference image for judging the noise); it resets when the camera moves.

## Verification the author must leave working
`build.cmd` builds without warnings-as-errors failures; running
`pw_bench12.exe 40 --gltf ..\bench\assets\lab_scene.glb --camera static --cam-dolly 2.9 --cam-lift 0.35 --fov 62 --sun-dir -0.30,-0.80,0.52 --sun-strength 400 --albedo 0.4 --dump 30`
prints `[info] bench finished after 40 frames, device ok` and writes dump_30.bmp. Enable the D3D12 debug layer with
`--debug-layer` and print its messages; a clean run has no errors.

## Style
C++20, no external libraries (Windows SDK only: d3d12, dxgi, dxguid, windowscodecs, ole32), ComPtr, comments in English that
explain WHY. Match the density and tone of comments in `tools/bench/pw_bench.cpp`. No `utils`/`helpers` catch-all files.
