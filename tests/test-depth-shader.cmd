@echo off
pushd "%~dp0.." || exit /b 1
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" || exit /b 1
cmake --preset windows-x64 || exit /b 1
cmake --build --preset windows-x64-release --target ofps_depth_shader_tests || exit /b 1
ctest --preset windows-x64-release -R "^ofps_depth_shader$" --output-on-failure || exit /b 1
popd
