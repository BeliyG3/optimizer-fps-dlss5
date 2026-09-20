@echo off
rem The interactive lab with our OptiScaler fork in the chain: path tracing -> DLSS RR -> Neural Rendering.
rem run_fork\ holds a copy of the bench next to the fork's dxgi.dll (see deploy_fork.cmd); Insert opens the
rem fork's menu, F1 the bench's own.
setlocal
call "%~dp0deploy_fork.cmd" || (pause & exit /b 1)
pushd "%~dp0run_fork" || exit /b 1
pw_bench12.exe --interactive --gltf ..\..\bench\assets\lab_scene.glb --upscaler rr --sun-dir 0.45,-0.77,0.45 --sun-strength 1500 --exposure 0.22 --haze 0.008 --fov 62 %*
set "result=%errorlevel%"
if not "%result%"=="0" pause
popd
exit /b %result%
