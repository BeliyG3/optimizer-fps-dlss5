@echo off
rem Standalone bench harness (tools\bench\run\pw_bench.exe).
rem pw_bench.cpp includes <nvsdk_ngx.h>: point PW_NGX_SDK_ROOT at the directory that holds it
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
cl /nologo /std:c++20 /EHsc /O2 /MD /W3 /I"%PW_NGX_SDK_ROOT%" pw_bench.cpp /Fe:run\pw_bench.exe /link d3d11.lib d3d12.lib dxgi.lib d3dcompiler.lib user32.lib kernel32.lib windowscodecs.lib ole32.lib
echo CL_RC=%ERRORLEVEL%
endlocal
