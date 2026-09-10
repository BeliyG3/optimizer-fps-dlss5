@echo off
rem Full x64 build (core + D3D adapters + shaders + tests + the ReShade add-on).
rem ReShade, Dear ImGui and Detours are fetched into external\ by cmake/Dependencies.cmake;
rem no local dependency paths are needed. Override with -DPW_RESHADE_SDK_ROOT=... etc. if wanted.
setlocal
call "%VSINSTALLDIR%VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1 || call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0..\.."
cmake --preset windows-x64 >nul || goto :fail
cmake --build --preset windows-x64-release 2>&1 | findstr /i /c:"error" /c:"warning" /c:"optimizer-fps-dlss5.addon64" /c:"Build FAILED"
echo BUILD_RC=%ERRORLEVEL%
ctest --preset windows-x64-release 2>&1 | findstr /i /c:"tests passed" /c:"Failed"
dir out\build\x64\adapters\reshade\Release\optimizer-fps-dlss5.addon64
endlocal
exit /b 0
:fail
echo CONFIGURE FAILED
endlocal
exit /b 1
