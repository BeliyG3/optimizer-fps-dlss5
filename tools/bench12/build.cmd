@echo off
setlocal
pushd "%~dp0" || exit /b 1
if not exist obj mkdir obj
set "DXC=C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\dxc.exe"
if not exist "%DXC%" (
  echo [fail] Install Windows SDK 10.0.26100.0: dxc.exe is missing.
  goto :fail
)
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VCVARS="
if exist "%VSWHERE%" (
  "%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath > obj\vs-path.txt
  for /f "usebackq delims=" %%V in ("obj\vs-path.txt") do set "VCVARS=%%V\VC\Auxiliary\Build\vcvars64.bat"
)
if not defined VCVARS (
  for %%E in (BuildTools Community Professional Enterprise) do if exist "%ProgramFiles%\Microsoft Visual Studio\2022\%%E\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%ProgramFiles%\Microsoft Visual Studio\2022\%%E\VC\Auxiliary\Build\vcvars64.bat"
)
if not defined VCVARS (
  echo [fail] Install Visual Studio C++ x64 build tools.
  goto :fail
)
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
call "%VCVARS%" || goto :fail
if not exist shaders\bin mkdir shaders\bin
if not exist obj mkdir obj
"%DXC%" -nologo -WX -Ges -O3 -T cs_6_5 -E CS -Fo shaders\bin\pathtrace.cso shaders\pathtrace.hlsl || goto :fail
"%DXC%" -nologo -WX -Ges -O3 -T vs_6_0 -E VS -Fo shaders\bin\present_vs.cso shaders\present.hlsl || goto :fail
"%DXC%" -nologo -WX -Ges -O3 -T ps_6_0 -E PS -Fo shaders\bin\present_ps.cso shaders\present.hlsl || goto :fail
"%DXC%" -nologo -WX -Ges -O3 -T cs_6_0 -E Downsample -Fo shaders\bin\display_down.cso shaders\display.hlsl || goto :fail
"%DXC%" -nologo -WX -Ges -O3 -T cs_6_0 -E Histogram -Fo shaders\bin\display_histogram.cso shaders\display.hlsl || goto :fail
"%DXC%" -nologo -WX -Ges -O3 -T cs_6_0 -E Adapt -Fo shaders\bin\display_adapt.cso shaders\display.hlsl || goto :fail
rem --local-exposure: the bilateral grid of the frame's log luminance, then its blur (local_exposure.cpp).
"%DXC%" -nologo -WX -Ges -O3 -T cs_6_0 -E Grid -Fo shaders\bin\local_exposure_grid.cso shaders\local_exposure.hlsl || goto :fail
"%DXC%" -nologo -WX -Ges -O3 -T cs_6_0 -E Blur -Fo shaders\bin\local_exposure_blur.cso shaders\local_exposure.hlsl || goto :fail
"%DXC%" -nologo -WX -Ges -O3 -T vs_6_5 -E VS -Fo shaders\bin\raster_vs.cso shaders\raster.hlsl || goto :fail
"%DXC%" -nologo -WX -Ges -O3 -T ps_6_5 -E PS -Fo shaders\bin\raster_ps.cso shaders\raster.hlsl || goto :fail
"%DXC%" -nologo -WX -Ges -O3 -T vs_6_5 -E VS -Fo shaders\bin\raster_shadow_vs.cso shaders\raster_shadow.hlsl || goto :fail
"%DXC%" -nologo -WX -Ges -O3 -T ps_6_5 -E PS -Fo shaders\bin\raster_shadow_ps.cso shaders\raster_shadow.hlsl || goto :fail
rem --lightmap: the same sources with the baked light (the default raster shaders above stay as they are).
"%DXC%" -nologo -WX -Ges -O3 -T vs_6_5 -E VS -D PW_LIGHTMAP=1 -Fo shaders\bin\raster_lightmap_vs.cso shaders\raster.hlsl || goto :fail
"%DXC%" -nologo -WX -Ges -O3 -T ps_6_5 -E PS -D PW_LIGHTMAP=1 -Fo shaders\bin\raster_lightmap_ps.cso shaders\raster.hlsl || goto :fail
"%DXC%" -nologo -WX -Ges -O3 -T vs_6_5 -E VS -D PW_LIGHTMAP=1 -Fo shaders\bin\raster_shadow_dynamic_vs.cso shaders\raster_shadow.hlsl || goto :fail
rem --csm: the sun shadow in cascades. The scene pass keeps raster_vs.cso and raster_lightmap_vs.cso (PW_CSM changes no vertex shader).
"%DXC%" -nologo -WX -Ges -O3 -T ps_6_5 -E PS -D PW_CSM=1 -Fo shaders\bin\raster_csm_ps.cso shaders\raster.hlsl || goto :fail
"%DXC%" -nologo -WX -Ges -O3 -T ps_6_5 -E PS -D PW_CSM=1 -D PW_LIGHTMAP=1 -Fo shaders\bin\raster_csm_lightmap_ps.cso shaders\raster.hlsl || goto :fail
"%DXC%" -nologo -WX -Ges -O3 -T vs_6_5 -E VS -D PW_CSM=1 -Fo shaders\bin\raster_csm_shadow_vs.cso shaders\raster_shadow.hlsl || goto :fail
"%DXC%" -nologo -WX -Ges -O3 -T vs_6_5 -E VS -D PW_CSM=1 -D PW_LIGHTMAP=1 -Fo shaders\bin\raster_csm_shadow_dynamic_vs.cso shaders\raster_shadow.hlsl || goto :fail
rem --ao: the depth/normal prepass (drawn with the scene pass's vertex shaders), the AO passes, the scene pass reading the AO.
"%DXC%" -nologo -WX -Ges -O3 -T ps_6_5 -E Prepass -Fo shaders\bin\raster_prepass_ps.cso shaders\raster_prepass.hlsl || goto :fail
"%DXC%" -nologo -WX -Ges -O3 -T ps_6_5 -E Prepass -D PW_LIGHTMAP=1 -Fo shaders\bin\raster_prepass_lightmap_ps.cso shaders\raster_prepass.hlsl || goto :fail
"%DXC%" -nologo -WX -Ges -O3 -T cs_6_5 -E CSAo -Fo shaders\bin\raster_ao.cso shaders\raster_ao.hlsl || goto :fail
"%DXC%" -nologo -WX -Ges -O3 -T cs_6_5 -E CSAoBlur -Fo shaders\bin\raster_ao_blur.cso shaders\raster_ao.hlsl || goto :fail
rem --ao game: the prepass with motion, 2 directions with per-frame noise, a 2x2 blur, the temporal filter.
"%DXC%" -nologo -WX -Ges -O3 -T ps_6_5 -E PrepassMotion -Fo shaders\bin\raster_prepass_motion_ps.cso shaders\raster_prepass.hlsl || goto :fail
"%DXC%" -nologo -WX -Ges -O3 -T ps_6_5 -E PrepassMotion -D PW_LIGHTMAP=1 -Fo shaders\bin\raster_prepass_motion_lightmap_ps.cso shaders\raster_prepass.hlsl || goto :fail
"%DXC%" -nologo -WX -Ges -O3 -T cs_6_5 -E CSAo -D PW_AO_GAME=1 -Fo shaders\bin\raster_ao_game.cso shaders\raster_ao.hlsl || goto :fail
"%DXC%" -nologo -WX -Ges -O3 -T cs_6_5 -E CSAoBlur -D PW_AO_GAME=1 -Fo shaders\bin\raster_ao_blur_game.cso shaders\raster_ao.hlsl || goto :fail
"%DXC%" -nologo -WX -Ges -O3 -T cs_6_5 -E CSAoTemporal -Fo shaders\bin\raster_ao_temporal.cso shaders\raster_ao_temporal.hlsl || goto :fail
"%DXC%" -nologo -WX -Ges -O3 -T ps_6_5 -E PS -D PW_AO=1 -Fo shaders\bin\raster_ao_ps.cso shaders\raster.hlsl || goto :fail
"%DXC%" -nologo -WX -Ges -O3 -T ps_6_5 -E PS -D PW_AO=1 -D PW_CSM=1 -Fo shaders\bin\raster_ao_csm_ps.cso shaders\raster.hlsl || goto :fail
"%DXC%" -nologo -WX -Ges -O3 -T ps_6_5 -E PS -D PW_AO=1 -D PW_LIGHTMAP=1 -Fo shaders\bin\raster_ao_lightmap_ps.cso shaders\raster.hlsl || goto :fail
"%DXC%" -nologo -WX -Ges -O3 -T ps_6_5 -E PS -D PW_AO=1 -D PW_CSM=1 -D PW_LIGHTMAP=1 -Fo shaders\bin\raster_ao_csm_lightmap_ps.cso shaders\raster.hlsl || goto :fail
rem --raster-sky-ambient: every scene pixel shader again with the ambient term from the HDRI sky's SH9 (the shaders above stay as they are).
"%DXC%" -nologo -WX -Ges -O3 -T ps_6_5 -E PS -D PW_SKYAMB=1 -Fo shaders\bin\raster_sky_ps.cso shaders\raster.hlsl || goto :fail
"%DXC%" -nologo -WX -Ges -O3 -T ps_6_5 -E PS -D PW_SKYAMB=1 -D PW_LIGHTMAP=1 -Fo shaders\bin\raster_sky_lightmap_ps.cso shaders\raster.hlsl || goto :fail
"%DXC%" -nologo -WX -Ges -O3 -T ps_6_5 -E PS -D PW_SKYAMB=1 -D PW_CSM=1 -Fo shaders\bin\raster_sky_csm_ps.cso shaders\raster.hlsl || goto :fail
"%DXC%" -nologo -WX -Ges -O3 -T ps_6_5 -E PS -D PW_SKYAMB=1 -D PW_CSM=1 -D PW_LIGHTMAP=1 -Fo shaders\bin\raster_sky_csm_lightmap_ps.cso shaders\raster.hlsl || goto :fail
"%DXC%" -nologo -WX -Ges -O3 -T ps_6_5 -E PS -D PW_SKYAMB=1 -D PW_AO=1 -Fo shaders\bin\raster_sky_ao_ps.cso shaders\raster.hlsl || goto :fail
"%DXC%" -nologo -WX -Ges -O3 -T ps_6_5 -E PS -D PW_SKYAMB=1 -D PW_AO=1 -D PW_CSM=1 -Fo shaders\bin\raster_sky_ao_csm_ps.cso shaders\raster.hlsl || goto :fail
"%DXC%" -nologo -WX -Ges -O3 -T ps_6_5 -E PS -D PW_SKYAMB=1 -D PW_AO=1 -D PW_LIGHTMAP=1 -Fo shaders\bin\raster_sky_ao_lightmap_ps.cso shaders\raster.hlsl || goto :fail
"%DXC%" -nologo -WX -Ges -O3 -T ps_6_5 -E PS -D PW_SKYAMB=1 -D PW_AO=1 -D PW_CSM=1 -D PW_LIGHTMAP=1 -Fo shaders\bin\raster_sky_ao_csm_lightmap_ps.cso shaders\raster.hlsl || goto :fail
rem --probes: the probe trace (once at start), and every scene pixel shader again with the ambient from the probes, with and without the sky's fallback.
"%DXC%" -nologo -WX -Ges -O3 -T cs_6_5 -E CS -Fo shaders\bin\probes_trace.cso shaders\probes_trace.hlsl || goto :fail
"%DXC%" -nologo -WX -Ges -O3 -T ps_6_5 -E PS -D PW_PROBES=1 -Fo shaders\bin\raster_probes_ps.cso shaders\raster.hlsl || goto :fail
"%DXC%" -nologo -WX -Ges -O3 -T ps_6_5 -E PS -D PW_PROBES=1 -D PW_LIGHTMAP=1 -Fo shaders\bin\raster_probes_lightmap_ps.cso shaders\raster.hlsl || goto :fail
"%DXC%" -nologo -WX -Ges -O3 -T ps_6_5 -E PS -D PW_PROBES=1 -D PW_CSM=1 -Fo shaders\bin\raster_probes_csm_ps.cso shaders\raster.hlsl || goto :fail
"%DXC%" -nologo -WX -Ges -O3 -T ps_6_5 -E PS -D PW_PROBES=1 -D PW_CSM=1 -D PW_LIGHTMAP=1 -Fo shaders\bin\raster_probes_csm_lightmap_ps.cso shaders\raster.hlsl || goto :fail
"%DXC%" -nologo -WX -Ges -O3 -T ps_6_5 -E PS -D PW_PROBES=1 -D PW_AO=1 -Fo shaders\bin\raster_probes_ao_ps.cso shaders\raster.hlsl || goto :fail
"%DXC%" -nologo -WX -Ges -O3 -T ps_6_5 -E PS -D PW_PROBES=1 -D PW_AO=1 -D PW_CSM=1 -Fo shaders\bin\raster_probes_ao_csm_ps.cso shaders\raster.hlsl || goto :fail
"%DXC%" -nologo -WX -Ges -O3 -T ps_6_5 -E PS -D PW_PROBES=1 -D PW_AO=1 -D PW_LIGHTMAP=1 -Fo shaders\bin\raster_probes_ao_lightmap_ps.cso shaders\raster.hlsl || goto :fail
"%DXC%" -nologo -WX -Ges -O3 -T ps_6_5 -E PS -D PW_PROBES=1 -D PW_AO=1 -D PW_CSM=1 -D PW_LIGHTMAP=1 -Fo shaders\bin\raster_probes_ao_csm_lightmap_ps.cso shaders\raster.hlsl || goto :fail
"%DXC%" -nologo -WX -Ges -O3 -T ps_6_5 -E PS -D PW_PROBES=1 -D PW_SKYAMB=1 -Fo shaders\bin\raster_probes_sky_ps.cso shaders\raster.hlsl || goto :fail
"%DXC%" -nologo -WX -Ges -O3 -T ps_6_5 -E PS -D PW_PROBES=1 -D PW_SKYAMB=1 -D PW_LIGHTMAP=1 -Fo shaders\bin\raster_probes_sky_lightmap_ps.cso shaders\raster.hlsl || goto :fail
"%DXC%" -nologo -WX -Ges -O3 -T ps_6_5 -E PS -D PW_PROBES=1 -D PW_SKYAMB=1 -D PW_CSM=1 -Fo shaders\bin\raster_probes_sky_csm_ps.cso shaders\raster.hlsl || goto :fail
"%DXC%" -nologo -WX -Ges -O3 -T ps_6_5 -E PS -D PW_PROBES=1 -D PW_SKYAMB=1 -D PW_CSM=1 -D PW_LIGHTMAP=1 -Fo shaders\bin\raster_probes_sky_csm_lightmap_ps.cso shaders\raster.hlsl || goto :fail
"%DXC%" -nologo -WX -Ges -O3 -T ps_6_5 -E PS -D PW_PROBES=1 -D PW_SKYAMB=1 -D PW_AO=1 -Fo shaders\bin\raster_probes_sky_ao_ps.cso shaders\raster.hlsl || goto :fail
"%DXC%" -nologo -WX -Ges -O3 -T ps_6_5 -E PS -D PW_PROBES=1 -D PW_SKYAMB=1 -D PW_AO=1 -D PW_CSM=1 -Fo shaders\bin\raster_probes_sky_ao_csm_ps.cso shaders\raster.hlsl || goto :fail
"%DXC%" -nologo -WX -Ges -O3 -T ps_6_5 -E PS -D PW_PROBES=1 -D PW_SKYAMB=1 -D PW_AO=1 -D PW_LIGHTMAP=1 -Fo shaders\bin\raster_probes_sky_ao_lightmap_ps.cso shaders\raster.hlsl || goto :fail
"%DXC%" -nologo -WX -Ges -O3 -T ps_6_5 -E PS -D PW_PROBES=1 -D PW_SKYAMB=1 -D PW_AO=1 -D PW_CSM=1 -D PW_LIGHTMAP=1 -Fo shaders\bin\raster_probes_sky_ao_csm_lightmap_ps.cso shaders\raster.hlsl || goto :fail
rem --lamp-tubes without --ao: a depth prepass, so the scene pass shades each pixel once.
"%DXC%" -nologo -WX -Ges -O3 -T ps_6_5 -E PrepassDepth -Fo shaders\bin\raster_tubes_prepass_ps.cso shaders\raster_prepass.hlsl || goto :fail
"%DXC%" -nologo -WX -Ges -O3 -T ps_6_5 -E PrepassDepth -D PW_LIGHTMAP=1 -Fo shaders\bin\raster_tubes_prepass_lightmap_ps.cso shaders\raster_prepass.hlsl || goto :fail
rem --lamp-tubes: every scene pixel shader above again with the lamp tubes as line lights (PW_TUBES; probes, sky, ao, csm, lightmap as above).
"%DXC%" -nologo -WX -Ges -O3 -T ps_6_5 -E PS -D PW_TUBES=1 -Fo shaders\bin\raster_tubes_ps.cso shaders\raster.hlsl || goto :fail
"%DXC%" -nologo -WX -Ges -O3 -T ps_6_5 -E PS -D PW_TUBES=1 -D PW_LIGHTMAP=1 -Fo shaders\bin\raster_tubes_lightmap_ps.cso shaders\raster.hlsl || goto :fail
"%DXC%" -nologo -WX -Ges -O3 -T ps_6_5 -E PS -D PW_TUBES=1 -D PW_CSM=1 -Fo shaders\bin\raster_tubes_csm_ps.cso shaders\raster.hlsl || goto :fail
"%DXC%" -nologo -WX -Ges -O3 -T ps_6_5 -E PS -D PW_TUBES=1 -D PW_CSM=1 -D PW_LIGHTMAP=1 -Fo shaders\bin\raster_tubes_csm_lightmap_ps.cso shaders\raster.hlsl || goto :fail
"%DXC%" -nologo -WX -Ges -O3 -T ps_6_5 -E PS -D PW_TUBES=1 -D PW_AO=1 -Fo shaders\bin\raster_tubes_ao_ps.cso shaders\raster.hlsl || goto :fail
"%DXC%" -nologo -WX -Ges -O3 -T ps_6_5 -E PS -D PW_TUBES=1 -D PW_AO=1 -D PW_LIGHTMAP=1 -Fo shaders\bin\raster_tubes_ao_lightmap_ps.cso shaders\raster.hlsl || goto :fail
"%DXC%" -nologo -WX -Ges -O3 -T ps_6_5 -E PS -D PW_TUBES=1 -D PW_AO=1 -D PW_CSM=1 -Fo shaders\bin\raster_tubes_ao_csm_ps.cso shaders\raster.hlsl || goto :fail
"%DXC%" -nologo -WX -Ges -O3 -T ps_6_5 -E PS -D PW_TUBES=1 -D PW_AO=1 -D PW_CSM=1 -D PW_LIGHTMAP=1 -Fo shaders\bin\raster_tubes_ao_csm_lightmap_ps.cso shaders\raster.hlsl || goto :fail
"%DXC%" -nologo -WX -Ges -O3 -T ps_6_5 -E PS -D PW_TUBES=1 -D PW_SKYAMB=1 -Fo shaders\bin\raster_tubes_sky_ps.cso shaders\raster.hlsl || goto :fail
"%DXC%" -nologo -WX -Ges -O3 -T ps_6_5 -E PS -D PW_TUBES=1 -D PW_SKYAMB=1 -D PW_LIGHTMAP=1 -Fo shaders\bin\raster_tubes_sky_lightmap_ps.cso shaders\raster.hlsl || goto :fail
"%DXC%" -nologo -WX -Ges -O3 -T ps_6_5 -E PS -D PW_TUBES=1 -D PW_SKYAMB=1 -D PW_CSM=1 -Fo shaders\bin\raster_tubes_sky_csm_ps.cso shaders\raster.hlsl || goto :fail
"%DXC%" -nologo -WX -Ges -O3 -T ps_6_5 -E PS -D PW_TUBES=1 -D PW_SKYAMB=1 -D PW_CSM=1 -D PW_LIGHTMAP=1 -Fo shaders\bin\raster_tubes_sky_csm_lightmap_ps.cso shaders\raster.hlsl || goto :fail
"%DXC%" -nologo -WX -Ges -O3 -T ps_6_5 -E PS -D PW_TUBES=1 -D PW_SKYAMB=1 -D PW_AO=1 -Fo shaders\bin\raster_tubes_sky_ao_ps.cso shaders\raster.hlsl || goto :fail
"%DXC%" -nologo -WX -Ges -O3 -T ps_6_5 -E PS -D PW_TUBES=1 -D PW_SKYAMB=1 -D PW_AO=1 -D PW_LIGHTMAP=1 -Fo shaders\bin\raster_tubes_sky_ao_lightmap_ps.cso shaders\raster.hlsl || goto :fail
"%DXC%" -nologo -WX -Ges -O3 -T ps_6_5 -E PS -D PW_TUBES=1 -D PW_SKYAMB=1 -D PW_AO=1 -D PW_CSM=1 -Fo shaders\bin\raster_tubes_sky_ao_csm_ps.cso shaders\raster.hlsl || goto :fail
"%DXC%" -nologo -WX -Ges -O3 -T ps_6_5 -E PS -D PW_TUBES=1 -D PW_SKYAMB=1 -D PW_AO=1 -D PW_CSM=1 -D PW_LIGHTMAP=1 -Fo shaders\bin\raster_tubes_sky_ao_csm_lightmap_ps.cso shaders\raster.hlsl || goto :fail
"%DXC%" -nologo -WX -Ges -O3 -T ps_6_5 -E PS -D PW_TUBES=1 -D PW_PROBES=1 -Fo shaders\bin\raster_tubes_probes_ps.cso shaders\raster.hlsl || goto :fail
"%DXC%" -nologo -WX -Ges -O3 -T ps_6_5 -E PS -D PW_TUBES=1 -D PW_PROBES=1 -D PW_LIGHTMAP=1 -Fo shaders\bin\raster_tubes_probes_lightmap_ps.cso shaders\raster.hlsl || goto :fail
"%DXC%" -nologo -WX -Ges -O3 -T ps_6_5 -E PS -D PW_TUBES=1 -D PW_PROBES=1 -D PW_CSM=1 -Fo shaders\bin\raster_tubes_probes_csm_ps.cso shaders\raster.hlsl || goto :fail
"%DXC%" -nologo -WX -Ges -O3 -T ps_6_5 -E PS -D PW_TUBES=1 -D PW_PROBES=1 -D PW_CSM=1 -D PW_LIGHTMAP=1 -Fo shaders\bin\raster_tubes_probes_csm_lightmap_ps.cso shaders\raster.hlsl || goto :fail
"%DXC%" -nologo -WX -Ges -O3 -T ps_6_5 -E PS -D PW_TUBES=1 -D PW_PROBES=1 -D PW_AO=1 -Fo shaders\bin\raster_tubes_probes_ao_ps.cso shaders\raster.hlsl || goto :fail
"%DXC%" -nologo -WX -Ges -O3 -T ps_6_5 -E PS -D PW_TUBES=1 -D PW_PROBES=1 -D PW_AO=1 -D PW_LIGHTMAP=1 -Fo shaders\bin\raster_tubes_probes_ao_lightmap_ps.cso shaders\raster.hlsl || goto :fail
"%DXC%" -nologo -WX -Ges -O3 -T ps_6_5 -E PS -D PW_TUBES=1 -D PW_PROBES=1 -D PW_AO=1 -D PW_CSM=1 -Fo shaders\bin\raster_tubes_probes_ao_csm_ps.cso shaders\raster.hlsl || goto :fail
"%DXC%" -nologo -WX -Ges -O3 -T ps_6_5 -E PS -D PW_TUBES=1 -D PW_PROBES=1 -D PW_AO=1 -D PW_CSM=1 -D PW_LIGHTMAP=1 -Fo shaders\bin\raster_tubes_probes_ao_csm_lightmap_ps.cso shaders\raster.hlsl || goto :fail
"%DXC%" -nologo -WX -Ges -O3 -T ps_6_5 -E PS -D PW_TUBES=1 -D PW_PROBES=1 -D PW_SKYAMB=1 -Fo shaders\bin\raster_tubes_probes_sky_ps.cso shaders\raster.hlsl || goto :fail
"%DXC%" -nologo -WX -Ges -O3 -T ps_6_5 -E PS -D PW_TUBES=1 -D PW_PROBES=1 -D PW_SKYAMB=1 -D PW_LIGHTMAP=1 -Fo shaders\bin\raster_tubes_probes_sky_lightmap_ps.cso shaders\raster.hlsl || goto :fail
"%DXC%" -nologo -WX -Ges -O3 -T ps_6_5 -E PS -D PW_TUBES=1 -D PW_PROBES=1 -D PW_SKYAMB=1 -D PW_CSM=1 -Fo shaders\bin\raster_tubes_probes_sky_csm_ps.cso shaders\raster.hlsl || goto :fail
"%DXC%" -nologo -WX -Ges -O3 -T ps_6_5 -E PS -D PW_TUBES=1 -D PW_PROBES=1 -D PW_SKYAMB=1 -D PW_CSM=1 -D PW_LIGHTMAP=1 -Fo shaders\bin\raster_tubes_probes_sky_csm_lightmap_ps.cso shaders\raster.hlsl || goto :fail
"%DXC%" -nologo -WX -Ges -O3 -T ps_6_5 -E PS -D PW_TUBES=1 -D PW_PROBES=1 -D PW_SKYAMB=1 -D PW_AO=1 -Fo shaders\bin\raster_tubes_probes_sky_ao_ps.cso shaders\raster.hlsl || goto :fail
"%DXC%" -nologo -WX -Ges -O3 -T ps_6_5 -E PS -D PW_TUBES=1 -D PW_PROBES=1 -D PW_SKYAMB=1 -D PW_AO=1 -D PW_LIGHTMAP=1 -Fo shaders\bin\raster_tubes_probes_sky_ao_lightmap_ps.cso shaders\raster.hlsl || goto :fail
"%DXC%" -nologo -WX -Ges -O3 -T ps_6_5 -E PS -D PW_TUBES=1 -D PW_PROBES=1 -D PW_SKYAMB=1 -D PW_AO=1 -D PW_CSM=1 -Fo shaders\bin\raster_tubes_probes_sky_ao_csm_ps.cso shaders\raster.hlsl || goto :fail
"%DXC%" -nologo -WX -Ges -O3 -T ps_6_5 -E PS -D PW_TUBES=1 -D PW_PROBES=1 -D PW_SKYAMB=1 -D PW_AO=1 -D PW_CSM=1 -D PW_LIGHTMAP=1 -Fo shaders\bin\raster_tubes_probes_sky_ao_csm_lightmap_ps.cso shaders\raster.hlsl || goto :fail
rem --lightmap with static triangles without lightmap UVs (veg_ foliage): the dynamic-only sun maps walk Scene::unbakedTriangles.
"%DXC%" -nologo -WX -Ges -O3 -T vs_6_5 -E VS -D PW_SHADOW_LIST=1 -Fo shaders\bin\raster_shadow_list_vs.cso shaders\raster_shadow.hlsl || goto :fail
"%DXC%" -nologo -WX -Ges -O3 -T vs_6_5 -E VS -D PW_CSM=1 -D PW_SHADOW_LIST=1 -Fo shaders\bin\raster_csm_shadow_list_vs.cso shaders\raster_shadow.hlsl || goto :fail
rem --lamp-shadows: the spot maps of every caster and of the triangles without baked light, then every scene pixel shader above
rem again with the lamp groups spot maps (PW_LAMPSHADOW; tubes, probes, sky, ao, csm, lightmap as above: :lampshadow_ps below).
"%DXC%" -nologo -WX -Ges -O3 -T vs_6_5 -E VS -D PW_LAMPSHADOW=1 -Fo shaders\bin\raster_lampshadow_vs.cso shaders\raster_shadow.hlsl || goto :fail
"%DXC%" -nologo -WX -Ges -O3 -T vs_6_5 -E VS -D PW_LAMPSHADOW=1 -D PW_SHADOW_LIST=1 -Fo shaders\bin\raster_lampshadow_list_vs.cso shaders\raster_shadow.hlsl || goto :fail
for %%T in (0 1) do for %%P in (0 1) do for %%S in (0 1) do for %%A in (0 1) do for %%C in (0 1) do for %%L in (0 1) do (
  call :lampshadow_ps %%T %%P %%S %%A %%C %%L || goto :fail
)
rem --sky-reflections: the sky's cube, its mips and the prefiltered reflection cube, built once at start (raster_reflections.cpp).
"%DXC%" -nologo -WX -Ges -O3 -T cs_6_5 -E Source -Fo shaders\bin\sky_source.cso shaders\sky_prefilter.hlsl || goto :fail
"%DXC%" -nologo -WX -Ges -O3 -T cs_6_5 -E Downsample -Fo shaders\bin\sky_downsample.cso shaders\sky_prefilter.hlsl || goto :fail
"%DXC%" -nologo -WX -Ges -O3 -T cs_6_5 -E Prefilter -Fo shaders\bin\sky_prefilter.cso shaders\sky_prefilter.hlsl || goto :fail
"%DXC%" -nologo -WX -Ges -O3 -T cs_6_5 -E Resolve -Fo shaders\bin\raster_resolve.cso shaders\raster_resolve.hlsl || goto :fail
rem --fxaa: the resolve again with FXAA 3.11 on the finished colour (PW_FXAA, shaders\raster_fxaa.hlsli); raster_resolve.cso stays as it is.
"%DXC%" -nologo -WX -Ges -O3 -T cs_6_5 -E Resolve -D PW_FXAA=1 -Fo shaders\bin\raster_fxaa_resolve.cso shaders\raster_resolve.hlsl || goto :fail
"%DXC%" -nologo -WX -Ges -O3 -T cs_6_5 -E ZeroMotion -Fo shaders\bin\raster_zero_motion.cso shaders\raster_resolve.hlsl || goto :fail
"%DXC%" -nologo -WX -Ges -O3 -T cs_6_0 -E Hud -Fo shaders\bin\hud.cso shaders\hud.hlsl || goto :fail
rem GPU skinning (the default; --cpu-skinning skins on the CPU as before).
"%DXC%" -nologo -WX -Ges -O3 -T cs_6_5 -E CS -Fo shaders\bin\skin.cso shaders\skin.hlsl || goto :fail
rem Culling of small instances: the Hi-Z, the occlusion pass, --cull-debug's overlay.
"%DXC%" -nologo -WX -Ges -O3 -T cs_6_5 -E CSHiZ -Fo shaders\bin\raster_hiz.cso shaders\raster_occlusion.hlsl || goto :fail
"%DXC%" -nologo -WX -Ges -O3 -T cs_6_5 -E CSOcclusion -Fo shaders\bin\raster_occlusion.cso shaders\raster_occlusion.hlsl || goto :fail
"%DXC%" -nologo -WX -Ges -O3 -T ps_6_5 -E CullDebug -Fo shaders\bin\raster_cull_debug_ps.cso shaders\raster_prepass.hlsl || goto :fail
cl /nologo /std:c++20 /EHsc /O2 /W0 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /Iexternal\imgui /Foobj\ /c external\imgui\imgui.cpp external\imgui\imgui_draw.cpp external\imgui\imgui_tables.cpp external\imgui\imgui_widgets.cpp external\imgui\backends\imgui_impl_win32.cpp external\imgui\backends\imgui_impl_dx12.cpp || goto :fail
rem A stale exe or forwarder must never survive a failed compile or link: delete them first, check them after.
call :remove pw_bench12.exe || goto :fail
call :remove nvngx.dll_pwbench12.dll || goto :fail
cl /nologo /std:c++20 /EHsc /O2 /W4 /WX /permissive- /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /Iexternal\imgui /Foobj\ /Fdobj\pw_bench12.pdb /I..\.. /I..\..\out\build\x64\generated /Fepw_bench12.exe ^
    core_model_host.cpp core_session.cpp ..\..\core\api\ofps_core_loader.cpp ngx_nr_audit.cpp main.cpp animation.cpp ^
    animation_pose.cpp scene_animation.cpp skin_layout.cpp gpu_skinning.cpp menu_animation.cpp application.cpp menu.cpp menu_render.cpp ^
    flight.cpp settings.cpp json.cpp lamps.cpp scene_lamps.cpp raster_lights.cpp device_resize.cpp ^
    pathtrace_resources.cpp pipeline.cpp display.cpp exposure_track.cpp local_exposure.cpp device_texture.cpp image_mips.cpp texture_cache.cpp device.cpp ^
    device_dump.cpp dump_writer.cpp raw_dump.cpp device_timing.cpp image.cpp scene.cpp scene_instances.cpp accel.cpp pathtrace.cpp raster_instances.cpp ^
    raster_cull.cpp raster_cull_passes.cpp raster_occlusion.cpp raster_occlusion_check.cpp ^
    pathtrace_pipeline.cpp wind.cpp raster.cpp raster_resources.cpp raster_pipeline.cpp raster_lightmap.cpp raster_csm.cpp raster_ao.cpp raster_sky.cpp raster_probes.cpp raster_tubes.cpp raster_lamp_shadows.cpp raster_lamp_shadow_passes.cpp raster_fxaa.cpp raster_reflections.cpp lamp_parts.cpp probes.cpp lightmap.cpp lightmap_file.cpp hud.cpp ngx.cpp ngx_evaluate.cpp ngx_nr.cpp ngx_nr_runtime.cpp ngx_nr_params.cpp ^
    ngx_nr_log.cpp ngx_nr_pad.cpp ngx_nr_release_thread.cpp ngx_nr_bridge.cpp motion_pack.cpp obj\imgui.obj obj\imgui_draw.obj ^
    obj\imgui_tables.obj obj\imgui_widgets.obj obj\imgui_impl_win32.obj obj\imgui_impl_dx12.obj /link /IMPLIB:obj\pw_bench12.lib ^
    d3dcompiler.lib dwmapi.lib d3d12.lib dxgi.lib dxguid.lib windowscodecs.lib ^
    ole32.lib user32.lib || goto :fail
rem nvngx_dlssnr.dll accepts calls only from a module whose path contains "nvngx.dll".
if not exist pw_bench12.exe (echo [fail] pw_bench12.exe was not built& goto :fail)
cl /nologo /std:c++20 /O2 /W4 /WX /permissive- /DWIN32_LEAN_AND_MEAN /DNOMINMAX /LD /Foobj\ /Fdobj\ngx_nr_forwarder.pdb /Fenvngx.dll_pwbench12.dll ngx_nr_forwarder.cpp /link /IMPLIB:obj\ngx_nr_forwarder.lib || goto :fail
if not exist nvngx.dll_pwbench12.dll (echo [fail] nvngx.dll_pwbench12.dll was not built& goto :fail)
cl /nologo /std:c++20 /EHsc /O2 /W4 /WX /permissive- /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /Foobj\ /Fdobj\tests.pdb /Feobj\bench12_tests.exe ^
    tests.cpp tests_nr.cpp ngx_nr_params.cpp tests_animation_perf.cpp tests_animation.cpp animation.cpp animation_pose.cpp tests_skinning.cpp skin_layout.cpp tests_materials.cpp tests_interactive.cpp tests_raster.cpp ^
    tests_raster_csm_ao.cpp tests_sky_sh.cpp tests_probes.cpp tests_tubes.cpp tests_lamp_shadows.cpp tests_fxaa.cpp tests_tonemap.cpp tests_reflections.cpp tests_dump.cpp tests_wind.cpp wind.cpp tests_foliage.cpp exposure_track.cpp tests_instances.cpp scene_instances.cpp tests_culling.cpp raster_cull.cpp raster_lights.cpp raster_lamp_shadows.cpp ^
    lamp_parts.cpp lightmap_file.cpp settings.cpp json.cpp lamps.cpp image_mips.cpp texture_cache.cpp image.cpp /link windowscodecs.lib ole32.lib user32.lib || goto :fail
obj\bench12_tests.exe || goto :fail
cl /nologo /std:c++20 /EHsc /O2 /W4 /WX /permissive- /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /I..\.. /I..\..\out\build\x64\generated /Foobj\ /Fdobj\core_session_tests.pdb /Feobj\core_session_tests.exe tests_core_session.cpp core_session.cpp core_model_host.cpp ..\..\core\api\ofps_core_loader.cpp ngx_nr_params.cpp ngx_nr_audit.cpp /link d3d12.lib dxgi.lib user32.lib || goto :fail
obj\core_session_tests.exe ..\..\out\build\x64\core\Release || goto :fail
echo [info] built pw_bench12.exe, nvngx.dll_pwbench12.dll and shaders
popd
exit /b 0
:fail
echo [fail] build failed
popd
exit /b 1
rem One --lamp-shadows scene pixel shader: the flags tubes, probes, sky, ao, csm, lightmap (0 or 1) name it and define it.
:lampshadow_ps
set "N=raster_lampshadow_"
set "F=-D PW_LAMPSHADOW=1"
if "%1"=="1" set "N=%N%tubes_"& set "F=%F% -D PW_TUBES=1"
if "%2"=="1" set "N=%N%probes_"& set "F=%F% -D PW_PROBES=1"
if "%3"=="1" set "N=%N%sky_"& set "F=%F% -D PW_SKYAMB=1"
if "%4"=="1" set "N=%N%ao_"& set "F=%F% -D PW_AO=1"
if "%5"=="1" set "N=%N%csm_"& set "F=%F% -D PW_CSM=1"
if "%6"=="1" set "N=%N%lightmap_"& set "F=%F% -D PW_LIGHTMAP=1"
"%DXC%" -nologo -WX -Ges -O3 -T ps_6_5 -E PS %F% -Fo shaders\bin\%N%ps.cso shaders\raster.hlsl
exit /b %errorlevel%
rem Deletes %1 if it exists; fails when it stays (a running bench holds it).
:remove
if exist %1 del /f /q %1
if exist %1 (echo [fail] %1 is in use: close the bench that runs it& exit /b 1)
exit /b 0
