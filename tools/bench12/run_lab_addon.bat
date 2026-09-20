@echo off
rem The interactive lab with the ReShade ADD-ON in the chain: path tracing -> DLSS RR -> renodx -> our add-on.
rem run_addon\ holds a copy of the bench next to ReShade, renodx and the add-on (see the add-on's bench notes);
rem Home opens the ReShade overlay with the "Optimizer FPS for DLSS5" tab, F1 the bench's own menu.
rem The lighting switches below are the scene's: without them the bench falls back to its defaults
rem (exposure 1.45 instead of 0.22) and the lab comes out blown out to white.
setlocal
pushd "%~dp0run_addon" || exit /b 1
pw_bench12.exe --interactive --gltf ..\..\bench\assets\lab_scene.glb --upscaler rr --sun-dir 0.45,-0.77,0.45 --sun-strength 1500 --exposure 0.22 --haze 0.008 --fov 62 %*
set "result=%errorlevel%"
if not "%result%"=="0" pause
popd
exit /b %result%
