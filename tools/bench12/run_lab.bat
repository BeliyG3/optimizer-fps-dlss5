@echo off
setlocal
pushd "%~dp0" || exit /b 1
pw_bench12.exe --interactive --gltf ..\bench\assets\lab_scene.glb --upscaler rr --sun-dir 0.45,-0.77,0.45 --sun-strength 1500 --exposure 0.22 --haze 0.008 --fov 62 %*
set "result=%errorlevel%"
if not "%result%"=="0" pause
popd
exit /b %result%
