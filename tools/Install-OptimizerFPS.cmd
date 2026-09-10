@echo off
setlocal
rem Optimizer FPS for DLSS5 -- installer launcher.
rem Copyright (c) 2026 Yuri Grib (BeliyG3). MIT licence (see LICENSE).
rem
rem Drag a game .exe (or its folder) onto this file, or run it from a command prompt:
rem     Install-OptimizerFPS.cmd "D:\Games\Baldurs Gate 3\bin\bg3_dx11.exe"

rem Pause at the end unless the caller already asked not to. When we pause, the script
rem itself is told not to, so a double-click never waits for Enter twice.
set "PW_PAUSE=1"
echo %*| find /i "-NoPause" >nul 2>&1
if not errorlevel 1 set "PW_PAUSE=0"

if "%PW_PAUSE%"=="1" (
    powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Install-OptimizerFPS.ps1" %* -NoPause
) else (
    powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Install-OptimizerFPS.ps1" %*
)
set "PW_CODE=%ERRORLEVEL%"

if "%PW_PAUSE%"=="1" (
    echo.
    pause
)
exit /b %PW_CODE%
