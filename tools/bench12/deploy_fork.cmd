@echo off
rem Refreshes run_fork\: the freshly built bench and its shaders next to the OptiScaler fork.
rem The fork's files (dxgi.dll, nvngx.dll_dlssnr.dll, nvngx_dlssnr.dll, OptiScaler.ini)
rem are taken from PW_FORK_SOURCE. Existing fork binaries and ini are preserved;
rem the core DLL and DXBC are refreshed from this repository's x64 build.
rem Delayed expansion: the default source path contains "(x86)", which would close a bracketed block.
setlocal EnableDelayedExpansion
set "HERE=%~dp0"
set "RUN=%HERE%run_fork"
if not defined PW_FORK_SOURCE set "PW_FORK_SOURCE=C:\Program Files (x86)\Steam\steamapps\common\007 First Light\Retail"
if not exist "!RUN!\shaders" mkdir "!RUN!\shaders"
copy /y "!HERE!pw_bench12.exe" "!RUN!\" >nul
if errorlevel 1 echo [fail] pw_bench12.exe is missing: run build.cmd& exit /b 1
xcopy /y /e /i /q "!HERE!shaders\bin" "!RUN!\shaders\bin" >nul
for %%F in (nvngx_dlss.dll nvngx_dlssd.dll) do if not exist "!RUN!\%%F" copy /y "!HERE!%%F" "!RUN!\" >nul
for %%F in (dxgi.dll nvngx.dll_dlssnr.dll nvngx_dlssnr.dll OptiScaler.ini) do call :fork_file "%%F" || exit /b 1
rem Refresh the shared core and shaders from this repository's current x64 build.
powershell -NoProfile -ExecutionPolicy Bypass -File "!HERE!stage_fork_core.ps1" -Runtime "!RUN!"
if errorlevel 1 exit /b 1
exit /b 0

:fork_file
if exist "!RUN!\%~1" exit /b 0
copy /y "!PW_FORK_SOURCE!\%~1" "!RUN!\" >nul
if errorlevel 1 echo [fail] %~1 not found in !PW_FORK_SOURCE!& exit /b 1
exit /b 0
