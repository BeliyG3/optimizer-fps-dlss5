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
"%DXC%" -nologo -WX -Ges -O3 -T cs_6_0 -E Adapt -Fo shaders\bin\display_adapt.cso shaders\display.hlsl || goto :fail
cl /nologo /std:c++20 /EHsc /O2 /W0 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /Iexternal\imgui /Foobj\ /c external\imgui\imgui.cpp external\imgui\imgui_draw.cpp external\imgui\imgui_tables.cpp external\imgui\imgui_widgets.cpp external\imgui\backends\imgui_impl_win32.cpp external\imgui\backends\imgui_impl_dx12.cpp || goto :fail
cl /nologo /std:c++20 /EHsc /O2 /W4 /WX /permissive- /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /Iexternal\imgui /Foobj\ /Fdobj\pw_bench12.pdb /I..\.. /I..\..\out\build\x64\generated /Fepw_bench12.exe core_model_host.cpp core_session.cpp ..\..\core\api\ofps_core_loader.cpp ngx_nr_audit.cpp main.cpp animation.cpp animation_pose.cpp scene_animation.cpp menu_animation.cpp application.cpp menu.cpp menu_render.cpp flight.cpp settings.cpp json.cpp lamps.cpp scene_lamps.cpp device_resize.cpp pathtrace_resources.cpp pipeline.cpp display.cpp device_texture.cpp image_mips.cpp device.cpp device_dump.cpp device_timing.cpp image.cpp scene.cpp accel.cpp pathtrace.cpp pathtrace_pipeline.cpp ngx.cpp ngx_evaluate.cpp ngx_nr.cpp ngx_nr_runtime.cpp ngx_nr_params.cpp ngx_nr_log.cpp ngx_nr_pad.cpp ngx_nr_bridge.cpp motion_pack.cpp obj\imgui.obj obj\imgui_draw.obj obj\imgui_tables.obj obj\imgui_widgets.obj obj\imgui_impl_win32.obj obj\imgui_impl_dx12.obj /link /IMPLIB:obj\pw_bench12.lib d3dcompiler.lib dwmapi.lib d3d12.lib dxgi.lib dxguid.lib windowscodecs.lib ole32.lib user32.lib || goto :fail
rem nvngx_dlssnr.dll accepts calls only from a module whose path contains "nvngx.dll".
cl /nologo /std:c++20 /O2 /W4 /WX /permissive- /DWIN32_LEAN_AND_MEAN /DNOMINMAX /LD /Foobj\ /Fdobj\ngx_nr_forwarder.pdb /Fenvngx.dll_pwbench12.dll ngx_nr_forwarder.cpp /link /IMPLIB:obj\ngx_nr_forwarder.lib || goto :fail
cl /nologo /std:c++20 /EHsc /O2 /W4 /WX /permissive- /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /Foobj\ /Fdobj\tests.pdb /Feobj\bench12_tests.exe tests.cpp tests_nr.cpp ngx_nr_params.cpp tests_animation_perf.cpp tests_animation.cpp animation.cpp animation_pose.cpp tests_materials.cpp tests_interactive.cpp settings.cpp json.cpp lamps.cpp image_mips.cpp image.cpp /link windowscodecs.lib ole32.lib user32.lib || goto :fail
obj\bench12_tests.exe || goto :fail
cl /nologo /std:c++20 /EHsc /O2 /W4 /WX /permissive- /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /I..\.. /I..\..\out\build\x64\generated /Foobj\ /Fdobj\core_session_tests.pdb /Feobj\core_session_tests.exe tests_core_session.cpp core_session.cpp core_model_host.cpp ..\..\core\api\ofps_core_loader.cpp ngx_nr_params.cpp ngx_nr_audit.cpp /link d3d12.lib dxgi.lib user32.lib || goto :fail
obj\core_session_tests.exe ..\..\out\build\x64\core\Release || goto :fail
echo [info] built pw_bench12.exe, nvngx.dll_pwbench12.dll and shaders
popd
exit /b 0
:fail
popd
exit /b 1
