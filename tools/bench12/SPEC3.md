# pw_bench12, milestone 3: materials, haze, display

Read `SPEC.md`, `SPEC2.md`, `README.md`, `NGX_PARAMETERS.md` and the sources first. Same rules: module layout and style kept,
new files under ~300 lines, README and tests updated, nothing outside `tools/bench12` is modified EXCEPT `../bench/pw_gltf.h`,
which may be extended in a backwards-compatible way (the D3D11 bench must still build and behave the same: new fields only,
defaults that reproduce today's behaviour).

## 1. Materials (the scene looks like velvet today: only the floor has a specular lobe)
- Every surface gets the specular lobe: dielectric F0 0.04, GGX with the material's roughness (clamp 0.04..1), mixture
  sampling diffuse/specular by Fresnel-weighted probability (clamp 0.1..0.9 so neither lobe starves).
- Metals: read `metallicFactor` and `metallicRoughnessTexture` (G = roughness, B = metallic) from the glTF material;
  F0 = lerp(0.04, baseColour, metallic), diffuse albedo scaled by (1 - metallic). Read `normalTexture` and apply tangent-space
  normal mapping (derive the tangent frame from the triangle's positions and UVs in the shader; no tangent attribute needed).
  Textures that are not sRGB colour (normal, metallic-roughness) must be created as UNORM, not SRGB.
  `pw_gltf.h` exposes today only the base-colour texture per primitive: add the two texture indices and the metallic factor
  to `Primitive`/`Vertex` (or a per-range material record), defaults -1 / 0.
- Bring back MIS for lamps hit by the BSDF sample (SPEC2 allowed dropping it; with near-mirror surfaces the reflections of the
  lamps are then lost, which is exactly what a polished floor must show). RIS next-event estimation and BSDF-sampled emission
  combined with MIS; document the weights. Emission seen through a specular bounce must appear at full strength.
- Guides follow: `specularAlbedo` from the env-BRDF fit with the real F0, `diffuseAlbedo` x (1 - metallic), shading normal in
  `normalRoughness`.
- Generate mips for all textures (a compute or blit chain, or CPU box filter at load) and sample with a ray-cone or a fixed
  distance-based LOD: distant monitors and floor textures alias badly at 960x540.

## 2. Haze (single scattering in a homogeneous medium; the light shafts of the reference game)
- `--haze D` extinction per metre (default 0.012, 0 = off), albedo 0.9, Henyey-Greenstein `--haze-g` 0.6.
- Per camera ray (and per indirect segment if cheap; camera ray only is acceptable): one distance sample by
  transmittance-proportional sampling up to the hit, in-scatter from the sun (one shadow ray) and from ONE RIS-selected lamp
  (one shadow ray), attenuate the surface radiance by the transmittance. 1 sample per pixel, noisy: RR denoises it.
- Haze is NOT described by motion vectors or depth (same as in a game): guides stay those of the surface behind.

## 3. Display
- Present pass: ACES-ish filmic tone map (keep the current one as `--tonemap neutral`), `--bloom S` (default 0.06): threshold-free
  bloom from a few downsampled blurred copies of the linear image, added before the tone map. Applies to what is presented and
  dumped, not to the buffers NGX sees.
- `--auto-exposure 0|1` (default 0): when 1, scale so the log-average luminance of the (upscaled) image maps to 0.18, smoothed
  over ~1 s. The scale is applied at presentation only; `--exposure` stays the scene-referred multiplier NGX sees.

## 4. Fixes left from milestone 2
- The process crashes (access violation) at exit after `--upscaler rr` has run ("bench finished" is printed first): release the
  NGX feature and shut NGX down while the device and queue are still alive and idle, before any D3D12 object NGX used is freed.
- `--upscaler sr`: `NVSDK_NGX_D3D12_Init` returns 0xBAD0000C while the same call succeeds for `rr`. Note the init path now
  uses the driver core's plain `NVSDK_NGX_D3D12_Init(appId, path, device, featureInfo, sdkVersion)` (see ngx.cpp). Find the
  difference between the two paths (it must be something done before init for sr) and fix it.
- `--debug-layer` with `--upscaler rr` must be clean; fix resource-state errors if your barriers are wrong (you cannot run it:
  re-read every transition around evaluate for symmetry and for the states the textures are created in).

## 5. Verification
Build with `cmd /c <full path>\build.cmd` (/W4 /WX), CPU tests pass, the D3D11 bench still compiles
(`tools/bench/build.cmd` needs an NGX SDK path you do not have: instead compile-check `pw_gltf.h` through your own tests).
List what you could not verify on a GPU.
