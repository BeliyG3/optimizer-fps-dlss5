# Exact NGX RR parameter writes

This audit enumerates every setter executed by the supplied D3D12 RR helpers, in call order, including null optional inputs, zero subrect bases, and repeated writes. Macro names below omit the common NVSDK_NGX_Parameter_ prefix. The literal key is exactly what [ngx parameter] prints. The implementation prints this full audit at creation and first evaluation.

Creation uses NVSDK_NGX_Feature_RayReconstruction (13), node masks 1, and the supplied SDK version. The plain driver-core initializer receives application ID `0x24480451` (not ProjectID). FeatureCommonInfo uses the executable directory for PathListInfo and application data. Those are initialization arguments, not parameter-map writes.

Milestone 3 keeps the parameter contract unchanged. Noisy colour includes camera haze and scene exposure. Diffuse albedo includes `(1-metallic)`, specular albedo uses the actual per-channel F0, and normals include normal mapping. Bloom, filmic tone mapping, and automatic display exposure run after evaluation and never enter NGX inputs or exposure parameters.

## CREATE (12 writes)

| Macro suffix | Literal key | Type | Value |
| --- | --- | --- | --- |
| CreationNodeMask | CreationNodeMask | UI | 1 |
| VisibilityNodeMask | VisibilityNodeMask | UI | 1 |
| Width | Width | UI | render width |
| Height | Height | UI | render height |
| OutWidth | OutWidth | UI | output width |
| OutHeight | OutHeight | UI | output height |
| PerfQualityValue | PerfQualityValue | I | nearest-ratio quality enum (0 at scale 0.5) |
| DLSS_Feature_Create_Flags | DLSS.Feature.Create.Flags | I | 3 standard depth / 11 reverse depth |
| DLSS_Enable_Output_Subrects | DLSS.Enable.Output.Subrects | I | 0 |
| DLSS_Denoise_Mode | DLSS.Denoise.Mode | I | DLUnified (1) |
| DLSS_Roughness_Mode | DLSS.Roughness.Mode | UI | Packed (1) |
| Use_HW_Depth | DLSS.Use.HW.Depth | UI | HW (1) |

## EVALUATE (156 writes)

| Macro suffix | Literal key | Type | Value |
| --- | --- | --- | --- |
| Color | Color | D3d12Resource | noisy colour, RGBA16F, render size |
| Output | Output | D3d12Resource | NGX output, RGBA16F, output size |
| Depth | Depth | D3d12Resource | unjittered hardware depth, R32F |
| MotionVectors | MotionVectors | D3d12Resource | current-to-previous pixels, RG16F |
| Jitter_Offset_X | Jitter.Offset.X | F | current Halton projection jitter X, render pixels (0 if disabled) |
| Jitter_Offset_Y | Jitter.Offset.Y | F | current Halton projection jitter Y, render pixels (0 if disabled) |
| Reset | Reset | I | 1 on first frame / camera cut, otherwise 0 |
| MV_Scale_X | MV.Scale.X | F | 1 |
| MV_Scale_Y | MV.Scale.Y | F | 1 |
| TransparencyMask | TransparencyMask | D3d12Resource | null |
| ExposureTexture | ExposureTexture | D3d12Resource | null |
| DLSS_Input_Bias_Current_Color_Mask | DLSS.Input.Bias.Current.Color.Mask | D3d12Resource | null |
| GBuffer_Albedo | GBuffer.Albedo | D3d12Resource | null |
| GBuffer_Roughness | GBuffer.Roughness | D3d12Resource | null |
| GBuffer_Metallic | GBuffer.Metallic | D3d12Resource | null |
| GBuffer_Specular | GBuffer.Specular | D3d12Resource | null |
| GBuffer_Subsurface | GBuffer.Subsurface | D3d12Resource | null |
| GBuffer_Normals | GBuffer.Normals | D3d12Resource | null |
| GBuffer_ShadingModelId | GBuffer.ShadingModelId | D3d12Resource | null |
| GBuffer_MaterialId | GBuffer.MaterialId | D3d12Resource | null |
| GBuffer_Atrrib_8 | GBuffer.Attrib.8 | D3d12Resource | null |
| GBuffer_Atrrib_9 | GBuffer.Attrib.9 | D3d12Resource | null |
| GBuffer_Atrrib_11 | GBuffer.Attrib.11 | D3d12Resource | null |
| GBuffer_Atrrib_12 | GBuffer.Attrib.12 | D3d12Resource | null |
| GBuffer_Atrrib_13 | GBuffer.Attrib.13 | D3d12Resource | null |
| GBuffer_Atrrib_14 | GBuffer.Attrib.14 | D3d12Resource | null |
| GBuffer_Atrrib_15 | GBuffer.Attrib.15 | D3d12Resource | null |
| TonemapperType | TonemapperType | UI | 0 |
| MotionVectors3D | MotionVectors3D | D3d12Resource | null |
| IsParticleMask | IsParticleMask | D3d12Resource | null |
| AnimatedTextureMask | AnimatedTextureMask | D3d12Resource | null |
| DepthHighRes | DepthHighRes | D3d12Resource | null |
| Position_ViewSpace | Position.ViewSpace | D3d12Resource | null |
| FrameTimeDeltaInMsec | FrameTimeDeltaInMsec | F | scripted: 1000/60; interactive: measured frame delta clamped to 1000/15 |
| RayTracingHitDistance | RayTracingHitDistance | D3d12Resource | null |
| GBuffer_SpecularMvec | GBuffer.SpecularMvec | D3d12Resource | null |
| DLSS_Input_Color_Subrect_Base_X | DLSS.Input.Color.Subrect.Base.X | UI | 0 |
| DLSS_Input_Color_Subrect_Base_Y | DLSS.Input.Color.Subrect.Base.Y | UI | 0 |
| DLSS_Input_Depth_Subrect_Base_X | DLSS.Input.Depth.Subrect.Base.X | UI | 0 |
| DLSS_Input_Depth_Subrect_Base_Y | DLSS.Input.Depth.Subrect.Base.Y | UI | 0 |
| DLSS_Input_MV_SubrectBase_X | DLSS.Input.MV.Subrect.Base.X | UI | 0 |
| DLSS_Input_MV_SubrectBase_Y | DLSS.Input.MV.Subrect.Base.Y | UI | 0 |
| DLSS_Input_Translucency_SubrectBase_X | DLSS.Input.Translucency.Subrect.Base.X | UI | 0 |
| DLSS_Input_Translucency_SubrectBase_Y | DLSS.Input.Translucency.Subrect.Base.Y | UI | 0 |
| DLSS_Input_Bias_Current_Color_SubrectBase_X | DLSS.Input.Bias.Current.Color.Subrect.Base.X | UI | 0 |
| DLSS_Input_Bias_Current_Color_SubrectBase_Y | DLSS.Input.Bias.Current.Color.Subrect.Base.Y | UI | 0 |
| DLSS_Output_Subrect_Base_X | DLSS.Output.Subrect.Base.X | UI | 0 |
| DLSS_Output_Subrect_Base_Y | DLSS.Output.Subrect.Base.Y | UI | 0 |
| DLSS_Render_Subrect_Dimensions_Width | DLSS.Render.Subrect.Dimensions.Width | UI | render width |
| DLSS_Render_Subrect_Dimensions_Height | DLSS.Render.Subrect.Dimensions.Height | UI | render height |
| DLSS_Pre_Exposure | DLSS.Pre.Exposure | F | 1 |
| DLSS_Exposure_Scale | DLSS.Exposure.Scale | F | 1 |
| DLSS_Indicator_Invert_X_Axis | DLSS.Indicator.Invert.X.Axis | I | 0 |
| DLSS_Indicator_Invert_Y_Axis | DLSS.Indicator.Invert.Y.Axis | I | 0 |
| GBuffer_Emissive | GBuffer.Emissive | D3d12Resource | null |
| DiffuseAlbedo | DLSS.Input.DiffuseAlbedo | D3d12Resource | diffuse albedo, RGBA16F |
| SpecularAlbedo | DLSS.Input.SpecularAlbedo | D3d12Resource | environment-BRDF albedo, RGBA16F |
| DLSS_Input_DiffuseAlbedo_Subrect_Base_X | DLSS.Input.DiffuseAlbedo.Subrect.Base.X | UI | 0 |
| DLSS_Input_DiffuseAlbedo_Subrect_Base_Y | DLSS.Input.DiffuseAlbedo.Subrect.Base.Y | UI | 0 |
| DLSS_Input_SpecularAlbedo_Subrect_Base_X | DLSS.Input.SpecularAlbedo.Subrect.Base.X | UI | 0 |
| DLSS_Input_SpecularAlbedo_Subrect_Base_Y | DLSS.Input.SpecularAlbedo.Subrect.Base.Y | UI | 0 |
| DLSS_Input_Normals_Subrect_Base_X | DLSS.Input.Normals.Subrect.Base.X | UI | 0 |
| DLSS_Input_Normals_Subrect_Base_Y | DLSS.Input.Normals.Subrect.Base.Y | UI | 0 |
| DLSS_Input_Roughness_Subrect_Base_X | DLSS.Input.Roughness.Subrect.Base.X | UI | 0 |
| DLSS_Input_Roughness_Subrect_Base_Y | DLSS.Input.Roughness.Subrect.Base.Y | UI | 0 |
| GBuffer_Normals | GBuffer.Normals | D3d12Resource | world normal xyz / roughness w, RGBA16F |
| GBuffer_Roughness | GBuffer.Roughness | D3d12Resource | null (packed normals.w) |
| DLSSD_Alpha | DLSSD.Alpha | D3d12Resource | null |
| DLSSD_OutputAlpha | DLSSD.OutputAlpha | D3d12Resource | null |
| DLSSD_ReflectedAlbedo | DLSSD.ReflectedAlbedo | D3d12Resource | null |
| DLSSD_ColorBeforeParticles | DLSSD.ColorBeforeParticles | D3d12Resource | null |
| DLSSD_ColorAfterParticles | DLSSD.ColorAfterParticles | D3d12Resource | null |
| DLSSD_ColorBeforeTransparency | DLSSD.ColorBeforeTransparency | D3d12Resource | null |
| DLSSD_ColorAfterTransparency | DLSSD.ColorAfterTransparency | D3d12Resource | null |
| DLSSD_ColorBeforeFog | DLSSD.ColorBeforeFog | D3d12Resource | null |
| DLSSD_ColorAfterFog | DLSSD.ColorAfterFog | D3d12Resource | null |
| DLSSD_ScreenSpaceSubsurfaceScatteringGuide | DLSSD.ScreenSpaceSubsurfaceScatteringGuide | D3d12Resource | null |
| DLSSD_ColorBeforeScreenSpaceSubsurfaceScattering | DLSSD.ColorBeforeScreenSpaceSubsurfaceScattering | D3d12Resource | null |
| DLSSD_ColorAfterScreenSpaceSubsurfaceScattering | DLSSD.ColorAfterScreenSpaceSubsurfaceScattering | D3d12Resource | null |
| DLSSD_ScreenSpaceRefractionGuide | DLSSD.ScreenSpaceRefractionGuide | D3d12Resource | null |
| DLSSD_ColorBeforeScreenSpaceRefraction | DLSSD.ColorBeforeScreenSpaceRefraction | D3d12Resource | null |
| DLSSD_ColorAfterScreenSpaceRefraction | DLSSD.ColorAfterScreenSpaceRefraction | D3d12Resource | null |
| DLSSD_DepthOfFieldGuide | DLSSD.DepthOfFieldGuide | D3d12Resource | null |
| DLSSD_ColorBeforeDepthOfField | DLSSD.ColorBeforeDepthOfField | D3d12Resource | null |
| DLSSD_ColorAfterDepthOfField | DLSSD.ColorAfterDepthOfField | D3d12Resource | null |
| DLSSD_DiffuseHitDistance | DLSSD.DiffuseHitDistance | D3d12Resource | null |
| DLSSD_SpecularHitDistance | DLSSD.SpecularHitDistance | D3d12Resource | primary specular sample hit distance, R16F |
| DLSSD_DiffuseRayDirection | DLSSD.DiffuseRayDirection | D3d12Resource | null |
| DLSSD_SpecularRayDirection | DLSSD.SpecularRayDirection | D3d12Resource | null |
| DLSSD_DiffuseRayDirectionHitDistance | DLSSD.DiffuseRayDirectionHitDistance | D3d12Resource | null |
| DLSSD_SpecularRayDirectionHitDistance | DLSSD.SpecularRayDirectionHitDistance | D3d12Resource | null |
| DLSSD_Alpha_Subrect_Base_X | DLSSD.Alpha.Subrect.Base.X | UI | 0 |
| DLSSD_Alpha_Subrect_Base_Y | DLSSD.Alpha.Subrect.Base.Y | UI | 0 |
| DLSSD_OutputAlpha_Subrect_Base_X | DLSSD.OutputAlpha.Subrect.Base.X | UI | 0 |
| DLSSD_OutputAlpha_Subrect_Base_Y | DLSSD.OutputAlpha.Subrect.Base.Y | UI | 0 |
| DLSSD_ReflectedAlbedo_Subrect_Base_X | DLSSD.ReflectedAlbedo.Subrect.Base.X | UI | 0 |
| DLSSD_ReflectedAlbedo_Subrect_Base_Y | DLSSD.ReflectedAlbedo.Subrect.Base.Y | UI | 0 |
| DLSSD_ColorAfterParticles_Subrect_Base_X | DLSSD.ColorAfterParticles.Subrect.Base.X | UI | 0 |
| DLSSD_ColorAfterParticles_Subrect_Base_Y | DLSSD.ColorAfterParticles.Subrect.Base.Y | UI | 0 |
| DLSSD_ColorBeforeParticles_Subrect_Base_X | DLSSD.ColorBeforeParticles.Subrect.Base.X | UI | 0 |
| DLSSD_ColorBeforeParticles_Subrect_Base_Y | DLSSD.ColorBeforeParticles.Subrect.Base.Y | UI | 0 |
| DLSSD_ColorBeforeTransparency_Subrect_Base_X | DLSSD.ColorBeforeTransparency.Subrect.Base.X | UI | 0 |
| DLSSD_ColorBeforeTransparency_Subrect_Base_Y | DLSSD.ColorBeforeTransparency.Subrect.Base.Y | UI | 0 |
| DLSSD_ColorAfterTransparency_Subrect_Base_X | DLSSD.ColorAfterTransparency.Subrect.Base.X | UI | 0 |
| DLSSD_ColorAfterTransparency_Subrect_Base_Y | DLSSD.ColorAfterTransparency.Subrect.Base.Y | UI | 0 |
| DLSSD_ColorAfterFog_Subrect_Base_X | DLSSD.ColorAfterFog.Subrect.Base.X | UI | 0 |
| DLSSD_ColorAfterFog_Subrect_Base_Y | DLSSD.ColorAfterFog.Subrect.Base.Y | UI | 0 |
| DLSSD_ColorBeforeFog_Subrect_Base_X | DLSSD.ColorBeforeFog.Subrect.Base.X | UI | 0 |
| DLSSD_ColorBeforeFog_Subrect_Base_Y | DLSSD.ColorBeforeFog.Subrect.Base.Y | UI | 0 |
| DLSSD_ScreenSpaceSubsurfaceScatteringGuide_Subrect_Base_X | DLSSD.ScreenSpaceSubsurfaceScatteringGuide.Subrect.Base.X | UI | 0 |
| DLSSD_ScreenSpaceSubsurfaceScatteringGuide_Subrect_Base_Y | DLSSD.ScreenSpaceSubsurfaceScatteringGuide.Subrect.Base.Y | UI | 0 |
| DLSSD_ColorBeforeScreenSpaceSubsurfaceScattering_Subrect_Base_X | DLSSD.ColorBeforeScreenSpaceSubsurfaceScattering.Subrect.Base.X | UI | 0 |
| DLSSD_ColorBeforeScreenSpaceSubsurfaceScattering_Subrect_Base_Y | DLSSD.ColorBeforeScreenSpaceSubsurfaceScattering.Subrect.Base.Y | UI | 0 |
| DLSSD_ColorAfterScreenSpaceSubsurfaceScattering_Subrect_Base_X | DLSSD.ColorAfterScreenSpaceSubsurfaceScattering.Subrect.Base.X | UI | 0 |
| DLSSD_ColorAfterScreenSpaceSubsurfaceScattering_Subrect_Base_Y | DLSSD.ColorAfterScreenSpaceSubsurfaceScattering.Subrect.Base.Y | UI | 0 |
| DLSSD_ScreenSpaceRefractionGuide_Subrect_Base_X | DLSSD.ScreenSpaceRefractionGuide.Subrect.Base.X | UI | 0 |
| DLSSD_ScreenSpaceRefractionGuide_Subrect_Base_Y | DLSSD.ScreenSpaceRefractionGuide.Subrect.Base.Y | UI | 0 |
| DLSSD_ColorBeforeScreenSpaceRefraction_Subrect_Base_X | DLSSD.ColorBeforeScreenSpaceRefraction.Subrect.Base.X | UI | 0 |
| DLSSD_ColorBeforeScreenSpaceRefraction_Subrect_Base_Y | DLSSD.ColorBeforeScreenSpaceRefraction.Subrect.Base.Y | UI | 0 |
| DLSSD_ColorAfterScreenSpaceRefraction_Subrect_Base_X | DLSSD.ColorAfterScreenSpaceRefraction.Subrect.Base.X | UI | 0 |
| DLSSD_ColorAfterScreenSpaceRefraction_Subrect_Base_Y | DLSSD.ColorAfterScreenSpaceRefraction.Subrect.Base.Y | UI | 0 |
| DLSSD_DepthOfFieldGuide_Subrect_Base_X | DLSSD.DepthOfFieldGuide.Subrect.Base.X | UI | 0 |
| DLSSD_DepthOfFieldGuide_Subrect_Base_Y | DLSSD.DepthOfFieldGuide.Subrect.Base.Y | UI | 0 |
| DLSSD_ColorBeforeDepthOfField_Subrect_Base_X | DLSSD.ColorBeforeDepthOfField.Subrect.Base.X | UI | 0 |
| DLSSD_ColorBeforeDepthOfField_Subrect_Base_Y | DLSSD.ColorBeforeDepthOfField.Subrect.Base.Y | UI | 0 |
| DLSSD_ColorAfterDepthOfField_Subrect_Base_X | DLSSD.ColorAfterDepthOfField.Subrect.Base.X | UI | 0 |
| DLSSD_ColorAfterDepthOfField_Subrect_Base_Y | DLSSD.ColorAfterDepthOfField.Subrect.Base.Y | UI | 0 |
| DLSSD_DiffuseHitDistance_Subrect_Base_X | DLSSD.DiffuseHitDistance.Subrect.Base.X | UI | 0 |
| DLSSD_DiffuseHitDistance_Subrect_Base_Y | DLSSD.DiffuseHitDistance.Subrect.Base.Y | UI | 0 |
| DLSSD_SpecularHitDistance_Subrect_Base_X | DLSSD.SpecularHitDistance.Subrect.Base.X | UI | 0 |
| DLSSD_SpecularHitDistance_Subrect_Base_Y | DLSSD.SpecularHitDistance.Subrect.Base.Y | UI | 0 |
| DLSSD_DiffuseRayDirection_Subrect_Base_X | DLSSD.DiffuseRayDirection.Subrect.Base.X | UI | 0 |
| DLSSD_DiffuseRayDirection_Subrect_Base_Y | DLSSD.DiffuseRayDirection.Subrect.Base.Y | UI | 0 |
| DLSSD_SpecularRayDirection_Subrect_Base_X | DLSSD.SpecularRayDirection.Subrect.Base.X | UI | 0 |
| DLSSD_SpecularRayDirection_Subrect_Base_Y | DLSSD.SpecularRayDirection.Subrect.Base.Y | UI | 0 |
| DLSSD_DiffuseRayDirectionHitDistance_Subrect_Base_X | DLSSD.DiffuseRayDirectionHitDistance.Subrect.Base.X | UI | 0 |
| DLSSD_DiffuseRayDirectionHitDistance_Subrect_Base_Y | DLSSD.DiffuseRayDirectionHitDistance.Subrect.Base.Y | UI | 0 |
| DLSSD_SpecularRayDirectionHitDistance_Subrect_Base_X | DLSSD.SpecularRayDirectionHitDistance.Subrect.Base.X | UI | 0 |
| DLSSD_SpecularRayDirectionHitDistance_Subrect_Base_Y | DLSSD.SpecularRayDirectionHitDistance.Subrect.Base.Y | UI | 0 |
| DLSS_WORLD_TO_VIEW_MATRIX | WorldToViewMatrix | VoidPointer | 16 row-major floats, transpose of unjittered bench view |
| DLSS_VIEW_TO_CLIP_MATRIX | ViewToClipMatrix | VoidPointer | 16 row-major floats, transpose of unjittered bench projection |
| DLSS_TransparencyLayer | DLSS.TransparencyLayer | D3d12Resource | null |
| DLSS_TransparencyLayerOpacity | DLSS.TransparencyLayerOpacity | D3d12Resource | null |
| DLSS_TransparencyLayerMvecs | DLSS.TransparencyLayerMvecs | D3d12Resource | null |
| DLSS_DisocclusionMask | DLSS.DisocclusionMask | D3d12Resource | null |
| DLSSD_ResponsivityMask | DLSSD.ResponsivityMask | D3d12Resource | null |
| DLSS_TransparencyLayer_Subrect_Base_X | DLSS.TransparencyLayer.Subrect.Base.X | UI | 0 |
| DLSS_TransparencyLayer_Subrect_Base_Y | DLSS.TransparencyLayer.Subrect.Base.Y | UI | 0 |
| DLSS_TransparencyLayerOpacity_Subrect_Base_X | DLSS.TransparencyLayerOpacity.Subrect.Base.X | UI | 0 |
| DLSS_TransparencyLayerOpacity_Subrect_Base_Y | DLSS.TransparencyLayerOpacity.Subrect.Base.Y | UI | 0 |
| DLSS_TransparencyLayerMvecs_Subrect_Base_X | DLSS.TransparencyLayerMvecs.Subrect.Base.X | UI | 0 |
| DLSS_TransparencyLayerMvecs_Subrect_Base_Y | DLSS.TransparencyLayerMvecs.Subrect.Base.Y | UI | 0 |
| DLSS_DisocclusionMask_Subrect_Base_X | DLSS.DisocclusionMask.Subrect.Base.X | UI | 0 |
| DLSS_DisocclusionMask_Subrect_Base_Y | DLSS.DisocclusionMask.Subrect.Base.Y | UI | 0 |
| DLSSD_ResponsivityMask_Subrect_Base_X | DLSSD.ResponsivityMask.Subrect.Base.X | UI | 0 |
| DLSSD_ResponsivityMask_Subrect_Base_Y | DLSSD.ResponsivityMask.Subrect.Base.Y | UI | 0 |

## Review uncertainties and limitations

- Header names/types and helper mappings are verified locally. Matrix row-vector convention follows the [NVIDIA nvpro DLSS RR sample](https://github.com/nvpro-samples/vk_denoise_dlssrr#matrices) because these supplied headers do not document it. Runtime matrix/jitter alignment still needs an RTX run.
- Packed roughness uses a null separate roughness input as requested. Specular misses/no specular sample use zero; distances above the R16F finite range saturate at 65504. Validate denoising behavior with the installed feature DLL.
- PreExposure/ExposureScale are 1 with already exposed noisy colour; no exposure texture or auto-exposure flag. This is deliberate but image quality has not been validated.
- Quality is selected by the nearest standard render ratio; a nonstandard ratio can be unsupported by the DLL. Frame delta is fixed simulation time in scripted runs and clamped wall time in interactive runs, independent of animation playback speed/pause.
- No optional guide buffers, preset hints, or undocumented NGX parameters are invented. The helper itself writes all optional null/zero fields shown above. Normal and roughness GBuffer keys are first cleared, then assigned by the dedicated inputs.

# DLSS Neural Rendering (feature 18), runtime nvngx_dlssnr.dll 310.8.0

Everything below was measured on 2026-09-21 (RTX 4080 SUPER, driver core r616, `nvngx_dlssnr.dll` 310.8.0
CL 38718415) with `--nr` and a separate probe, from the runtime's own log. Items marked *inferred* are
conclusions from those results, not documented behaviour.

## Hosting

- The NGX core cannot create feature 18 with this runtime. Its Authenticode hash does not verify
  (`Get-AuthenticodeSignature`: HashMismatch; every copy in this repository is the same file); the core logs
  `nvLoadSignedLibraryW() failed on snippet '...nvngx_dlssnr.dll' ... The digital signature of the object did
  not verify`, capability parameters report `DLSSNR.Available 0`, `DLSSNR.FeatureInitResult 0xBAD00004`, and
  `NVSDK_NGX_D3D12_CreateFeature(list, 18, ...)` returns `0xBAD0000B`.
- The bench therefore loads the DLL itself and calls its exports (`NVSDK_NGX_D3D12_Init_Ext`,
  `PopulateParameters_Impl`, `CreateFeature`, `EvaluateFeature`, `ReleaseFeature`) through
  `nvngx.dll_pwbench12.dll`: the runtime accepts a call only if the return address lies in a module whose path
  contains `nvngx.dll`. `Init_Ext` gets application id `0x24480451`, the executable directory as data path,
  the bench's device, SDK version `0x15` and an empty host parameter block.
- The parameter block is the bench's own `NgxParameterMap` (numeric getters convert between types). The
  Optimizer FPS add-on finds its float setter/getter at vtable slots 6/14 in it, as in the core's block.
- Logging: `__NGX_LOG_LEVEL=1` (set by the bench only around the runtime's load and init) makes the runtime
  write `nvngx_dlssnr_310_8_0.log` into the data path; level 1 already contains create details and every
  `Skip feature evaluate` line. The runtime also prints the same lines through its own buffered stdout, so in
  runs with many refusals they appear in the console in chunks. The runtime has no app log callback here
  (`PollRuntimeParams - callback is NULL (core did not set it)` is expected).

## What the runtime reads

| When | Key | Getter type |
| --- | --- | --- |
| create | `ResourceAllocCallback`, `ResourceReleaseCallback` (optional) | pointer |
| create | `CreationNodeMask`, `VisibilityNodeMask`, `DLSSNR.Width`, `DLSSNR.Height` | unsigned |
| create | `DLSSNR.ScalingRatio` | float |
| create | `DLSSNR.Hint.Render.Preset` | int |
| evaluate | `DLSSNR.Color`, `.MVec`, `.Depth`, `.Output` | pointer |
| evaluate | `DLSSNR.<Color/MVec/Depth/Output>Subrect<BaseX/BaseY/Width/Height>` | int |
| evaluate | `DLSSNR.ControlMask`, `.UI`, `.UIAlpha`, `.Backbuffer`, `.BidirectionalDistortionField` (optional) | pointer |
| evaluate | `DLSSNR.MVecScaleX/Y`, `.Intensity`, `.ScalingRatio`, `.LocalToneStrength`, `.LocalStructureStrength`, `.SkinStructureStrength` | float |
| evaluate | `DLSSNR.UseAutoMask`, `.Reset`, `.DepthInverted`, `.Enabled`, `.UICorrection`, `DLSS.Indicator.Invert.X/Y.Axis` | int |
| evaluate | `DLSSNR.Style` | unsigned |

`PerfQualityValue` is read only by `DLSSNRComputeScalingRatioCallback` (published by `PopulateParameters_Impl`
together with `DLSSNRGetStatsCallback`), never by create or evaluate:

| PerfQualityValue | Callback result |
| --- | --- |
| 0 MaxPerf, 1 Balanced, 2 MaxQuality, 4 UltraQuality, 5 DLAA | `DLSSNR.ScalingRatio = 1.0` |
| 3 UltraPerformance, 6, 7, 8, 9 | `0xBAD00010`, log `Error: unsupported PerfQualityValue %u for DLSSNR scaling ratio computation` |

Only one network is embedded (`CC_Control_History_Blend_Quantize_With_Teacher_honest_tench_2026_07_04_22_30`);
preset 0 logs `preset 0 is not available in this DLL build; falling back to shipping default preset 1`.

## Native contract (works)

Create (`--nr native`, after DLSS SR at 1920x1080):

| Key | Value |
| --- | --- |
| `CreationNodeMask`, `VisibilityNodeMask` | 1 |
| `DLSSNR.Width` / `DLSSNR.Height` | colour size, 1920 / 1080 (runtime: `requested resolution 1920x1080 (network 1920x1080)`) |
| `DLSSNR.Hint.Render.Preset` | 1 |

Evaluate (per frame; the first evaluate prints the full list as `[nr parameter]`):

| Key | Value |
| --- | --- |
| `DLSSNR.Color` | 1920x1080 RGBA16F, `NON_PIXEL_SHADER_RESOURCE`; sub-rect 0,0 1920x1080 |
| `DLSSNR.Depth` | 960x540 R32F (render size); sub-rect 0,0 960x540 |
| `DLSSNR.MVec` | 960x540 RG16F or RGBA16F (`--mv-format`); sub-rect 0,0 960x540 |
| `DLSSNR.Output` | separate RGBA16F UAV, `UNORDERED_ACCESS`; sub-rect 0,0 1920x1080 (or at the pad base) |
| `DLSSNR.MVecScaleX/Y` | the DLSS scales: 1,1 for `--motion pixels`, (-w/2, h/2) for `ndc` |
| `DLSSNR.DepthInverted` | 1 for `--depth reverse`, else 0 |
| `DLSSNR.Reset` | 1 on the first NR frame and on camera cuts |
| `DLSSNR.Enabled` 1, `DLSSNR.Style` 0, `UseAutoMask` 0, `UICorrection` 0 | |
| `Intensity`, `LocalToneStrength`, `LocalStructureStrength`, `SkinStructureStrength` | 2 (renodx-dlss5 defaults) |

Result `0x00000001`. The sub-rects are mandatory: without them the runtime sees 0x0 rects and returns
`0xBAD00005` (`Invalid Color/Output rect configuration ... subrect=(0,0 0x0)`).

Colour: the model expects a finished SDR frame. Linear HDR colour (values above 1, dark linear values) gives a
red-magenta, blocky image; an sRGB-encoded proxy (white point 1.0, soft knee above 0.75 luminance) gives a
plausible one, and the output is decoded back (`--nr-colour srgb`, default). The probe shows the output
clamped to 1.0 (input red 0..2 came back with a maximum of 1.0).

RGBA16F motion (xy motion, zw 0) is accepted by DLSS SR, RR and NR; SR+NR dumps are bit-identical to RG16F.

## Upscaling (every tested combination refused)

`--nr upscale` creates the feature with `DLSSNR.Width/Height` = render size 960x540, `PerfQualityValue` 0 and
`DLSSNR.ScalingRatio` 0.5, colour and guides 960x540, output 1920x1080. Evaluate returns `0xBAD00005`:
`DLSSNR: Skip feature evaluate: Invalid Color/Output rect configuration Color=... subrect=(0,0 960x540)
Output=... subrect=(0,0 1920x1080)`. The bench presents the render colour instead.

| Width/Height | ScalingRatio (create / evaluate) | PerfQualityValue | Colour | Output | Result |
| --- | --- | --- | --- | --- | --- |
| 960x540 | - / - | - | 960x540 | 1920x1080 | `0xBAD00005`, rect message |
| 1920x1080 | - / - | - | 960x540 | 1920x1080 | same |
| 1920x1080 | 0.5, 2 / -, 0.5 | - | 960x540 | 1920x1080 | same |
| 960x540 | 0.5, 2 / -, 0.5, 2 | - | 960x540 | 1920x1080 | same |
| 1920x1080 | 0.5 / 0.5 | 0 (+ preset 1) | 960x540 | 1920x1080 | same |
| 1920x1080 | - / - | 1, 5 | 960x540 | 1920x1080 | same |
| 1920x1080 | - / - | - | 960x540 + Backbuffer 1920x1080 | 1920x1080 | same; Backbuffer untouched |
| 960x540 | - / - | - | 960x540 | 960x540 + Backbuffer 1920x1080 | `0x1`, 960x540 written, Backbuffer untouched |
| 1920x1080 | - / - | - | 1920x1080 texture, sub-rect 960x540 | 1920x1080 | `0x1`, only the top-left 960x540 of the output written |
| 1920x1080 | - / - | - | 1920x1080 | 1920x1080 texture, sub-rect 960x540 | `0xBAD00005`, rect message |
| 960x540 | - / - | - | 960x540 | 1920x1080 texture, sub-rect 960x540 | `0x1`, native inside the output |

`DLSSNR.ScalingRatio` is read at create and evaluate but has no visible effect for 0.25, 0.3333, 0.5, 0.6667,
0.9, 1.5, 2 or 3: the network always equals the output sub-rect (created 960x540 with a 1920x1080 output logs
`Network size grew past feature size for feature 1 (960x540 -> 1920x1080); resizing internal capacity`).
*Inferred:* 310.8 has no working upscaling path; colour and output must share one pixel grid (the colour
sub-rect fits in the output sub-rect, and the output sub-rect fits in the colour texture).

## Padded output (`--nr-output-pad`)

Output 1984x1112 with the 1920x1080 region at 32,16: accepted, only the region is written. Region at 0,0:
accepted, but the runtime also writes the 1920x32 band below the region (region width times texture height,
grey around 0.49; the right pad stays untouched). The region image is bit-identical to an unpadded run in
both cases.
