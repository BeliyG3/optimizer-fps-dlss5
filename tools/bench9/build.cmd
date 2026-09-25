@echo off
rem Builds pw_bench9.exe, the 32-bit Direct3D 9 bench (x86 toolchain; Visual Studio 2022).
setlocal
call "%VSINSTALLDIR%VC\Auxiliary\Build\vcvars32.bat" >nul 2>&1 || call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat" >nul
cd /d "%~dp0"
if not exist obj mkdir obj
cl /nologo /O2 /EHsc /W4 /WX /MT /Foobj\ /Fe:pw_bench9.exe pw_bench9.cpp bench9_hud.cpp d3d9.lib user32.lib gdi32.lib
echo BUILD_RC=%ERRORLEVEL%
endlocal
