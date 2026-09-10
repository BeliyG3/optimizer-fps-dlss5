@echo off
rem 32-bit remote overlay add-on only (optimizer-fps-dlss5-remote.addon32).
rem The preset uses the Visual Studio generator with -A Win32, so the x64 developer
rem prompt is enough; vcvars32/vcvarsall x86 works as well.
setlocal
call "%VSINSTALLDIR%VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1 || call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0..\.."
cmake --preset windows-x86-remote >nul || goto :fail
cmake --build --preset windows-x86-remote-release 2>&1 | findstr /i /c:"error" /c:"warning" /c:"optimizer-fps-dlss5-remote.addon32" /c:"Build FAILED"
echo BUILD_RC=%ERRORLEVEL%
dir out\build\x86-remote\adapters\reshade\Release\optimizer-fps-dlss5-remote.addon32
endlocal
exit /b 0
:fail
echo CONFIGURE FAILED
endlocal
exit /b 1
