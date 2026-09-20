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
