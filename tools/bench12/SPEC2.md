# pw_bench12, milestone 2: game-like sampling, DLSS Super Resolution and Ray Reconstruction

Read `SPEC.md`, `README.md` and the existing sources first. Keep the module layout and style; every new file under
~300 lines; update `README.md` and `tests.cpp`. Do not touch anything outside `tools/bench12`.

## 1. Sampling like a path-traced game (same ray density: ONE path per pixel at render resolution)
- Default `--bounces 4` (range 0..8 stays).
- Lamps: keep ONE shadow ray per path vertex, but pick the lamp with resampled importance sampling (what RTXDI does at
  its minimum): draw M = 8 candidates from the power CDF, weight each by its unshadowed contribution
  (emission luminance x geometry term x BSDF x cosine / source pdf), select one proportionally, trace one shadow ray,
  and weight by (sum of weights / M) / selected target. Keep the MIS against the BSDF sample unbiased or drop MIS for
  the lamp term and do not add emission on BSDF hits of lamps after the first vertex - whichever you choose must be
  unbiased; say which in a comment. `--light-candidates M` (1 = the old behaviour).
- Emissive surfaces with a base-colour texture (monitor screens): radiance = emission x texture colour, both when seen
  directly and when sampled as a lamp (use the texture at the sampled point).
- `--vsync 0|1` (default 0, present with tearing when supported) and a per-run report:
  `[info] gpu frame time: avg X ms (path trace Y ms)` from timestamp queries.

## 2. NGX: `ngx.h/.cpp`
- Load `_nvngx.dll` from the NVIDIA driver store: enumerate `C:\Windows\System32\DriverStore\FileRepository\nv_dispi.inf_amd64_*`
  and take the first that has `_nvngx.dll` (the D3D11 bench hardcodes one path; see `tools/bench/pw_bench.cpp` ~line 1332 for the
  call pattern, entry points there are the D3D11 ones). Here use `NVSDK_NGX_D3D12_Init` (or `_Init_with_ProjectID`),
  `NVSDK_NGX_D3D12_GetCapabilityParameters` / `AllocateParameters`, `CreateFeature`, `EvaluateFeature`, `ReleaseFeature`,
  `Shutdown1`, resolved with GetProcAddress (no import library exists here). Headers: `external/ngx/*.h` (NVIDIA DLSS SDK,
  already downloaded; `nvsdk_ngx_helpers.h`, `nvsdk_ngx_helpers_dlssd.h`). The helper inline functions call
  `NVSDK_NGX_D3D12_CreateFeature/EvaluateFeature` by name: provide those two symbols yourself as thin forwarders to the
  GetProcAddress pointers so the helpers link.
- The feature DLLs (`nvngx_dlss.dll`, `nvngx_dlssd.dll`) are looked up next to the exe; pass the exe directory as the
  application data path and in `NVSDK_NGX_FeatureCommonInfo.PathListInfo`. If a DLL is missing, print a clear message.
- `--upscaler none|sr|rr` (default none), `--quality` is implied by `--render-scale` (0.5 = Performance).
  - `sr`: DLSS Super Resolution. Input: the noisy colour (yes, it will look bad - it is the baseline), depth, motion.
  - `rr`: DLSS Ray Reconstruction (`NVSDK_NGX_Feature_RayReconstruction`), `NGX_D3D12_CREATE_DLSSD_EXT` /
    `NGX_D3D12_EVALUATE_DLSSD_EXT`: colour (noisy linear HDR), output (RGBA16F at output size), depth, motion vectors
    (render-size, pixel units, `InMVScaleX/Y = 1`, current->previous), jitter offsets in pixels, `InRenderSubrectDimensions`,
    `pInDiffuseAlbedo`, `pInSpecularAlbedo`, `pInNormals`, `pInRoughness` (roughness is packed in normals.w:
    use `NVSDK_NGX_DLSS_Roughness_Mode_Packed`; otherwise write a separate R16F), `pInSpecularHitDistance` with
    `pInWorldToViewMatrix`/`pInViewToClipMatrix` (row-major float[16], check the header comment for the convention),
    `InReset` on the first frame and on camera cuts, `InFrameTimeDeltaInMsec`.
    Create flags: `IsHDR`, `MVLowRes`, and `DepthInverted` only with `--depth reverse`; denoise mode DLUnified.
  - Resource states: transition every input to the state the DLSS programming guide expects before evaluate
    (inputs `NON_PIXEL_SHADER_RESOURCE`, output `UNORDERED_ACCESS`) and restore after; NGX changes the command list's
    descriptor heaps and root signature: re-set them after evaluate.
- Present shows the upscaler output (tone-mapped) when one is active; `--view` still selects debug views. `--dump` dumps what
  is presented. `--hdr-out 1` skips the tone mapper in the dump (writes an .hdr-less linear clamp) - optional.
- Specular hit distance: write the real distance of the specular BSDF sample at the primary hit when that sample was taken,
  else 0.
- Specular albedo: use the environment-BRDF approximation of F0 0.04 and roughness (the DLSS RR guide gives the formula) instead
  of a flat 0.04.

## 3. Verification you must leave
`build.cmd` builds with /W4 /WX as before and the CPU tests pass. You cannot run the GPU. List precisely which NGX parameters
you set for RR and which you were unsure about, so the reviewer can check them against the log.
