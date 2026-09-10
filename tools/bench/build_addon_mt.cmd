@echo off
rem x64 add-on linked against the static CRT (/MT): no MSVCP140/VCRUNTIME140 in the game process.
setlocal
call "%VSINSTALLDIR%VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1 || call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0..\.."
cmake --preset windows-x64-mt >nul || goto :fail
cmake --build --preset windows-x64-mt-release 2>&1 | findstr /i /c:"error" /c:"optimizer-fps-dlss5.addon64" /c:"Build FAILED"
echo BUILD_RC=%ERRORLEVEL%
dir out\build\x64-mt\adapters\reshade\Release\optimizer-fps-dlss5.addon64
echo ==== dependents (expect no VCRUNTIME/MSVCP)
dumpbin /dependents out\build\x64-mt\adapters\reshade\Release\optimizer-fps-dlss5.addon64 | findstr /i /c:".dll"
endlocal
exit /b 0
:fail
echo CONFIGURE FAILED
endlocal
exit /b 1
