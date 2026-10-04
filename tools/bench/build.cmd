@echo off
rem Standalone bench harness (tools\bench\run\pw_bench.exe), built from pw_bench.cpp (entry point) and the
rem bench_*.cpp modules beside it; pw_gltf.h is header-only.
rem bench_ngx.h includes <nvsdk_ngx.h>: point PW_NGX_SDK_ROOT at the directory that holds it
rem (the NVIDIA DLSS SDK "include" directory), e.g.
rem     set PW_NGX_SDK_ROOT=C:\src\nvngx_dlss_sdk
setlocal
call "%VSINSTALLDIR%VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1 || call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0"
if "%PW_NGX_SDK_ROOT%"=="" (
    echo PW_NGX_SDK_ROOT is not set - it must point at the directory containing nvsdk_ngx.h
    endlocal
    exit /b 1
)
if not exist "run" mkdir run
set SOURCES=pw_bench.cpp bench_addon.cpp bench_boxes.cpp bench_camera.cpp bench_capture.cpp bench_device.cpp bench_frame.cpp
set SOURCES=%SOURCES% bench_gltf_scene.cpp bench_hdri.cpp bench_lighting.cpp bench_ngx.cpp bench_options.cpp bench_pipeline.cpp
set SOURCES=%SOURCES% bench_render.cpp bench_shaders.cpp bench_timing.cpp
cl /nologo /std:c++20 /EHsc /O2 /MD /W3 /I"%PW_NGX_SDK_ROOT%" %SOURCES% /Fe:run\pw_bench.exe /link d3d11.lib d3d12.lib dxgi.lib d3dcompiler.lib user32.lib kernel32.lib windowscodecs.lib ole32.lib
echo CL_RC=%ERRORLEVEL%
endlocal
