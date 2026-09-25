# Build and validate the current source tree

Requirements: Visual Studio 2022 Community with the C++ workload and Windows SDK,
CMake 3.25 or newer, Python 3, and the pinned add-on dependencies described in
[docs/BUILD.md](docs/BUILD.md). Run these commands from this repository's root.

From a regular command prompt, initialize MSVC before CMake and CTest.
Use this command for each preset; substitute the preset name:

```cmd
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
cmake --preset windows-x64
cmake --build --preset windows-x64-release --parallel
ctest --preset windows-x64-release --output-on-failure
```

The other build presets are `windows-x64-mt`, `windows-x86`,
`windows-x86-remote`, and `sdk-only-x64`. Each uses `<preset>-release` for the
build. CTest presets exist for `windows-x64-release`, `windows-x86-release`,
and `sdk-only-x64-release`; their current inventories are 26, 13, and 10 tests.
All five Windows builds enable warnings as errors. See [docs/BUILD.md](docs/BUILD.md)
for the artifact paths and SDK install instructions.

Run the source guards and PowerShell 5.1 lint from the repository root:

```powershell
python tools/check-abi-freeze.py
python tools/check-core-includes.py
python tools/check-file-size.py --changed
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/Lint-PowerShell51.ps1 -Path tools
```

The x64 runtime payload is `out/build/x64/hosts/reshade/Release/optimizer-fps-dlss5.addon64`,
`out/build/x64/hosts/reshade/ngx_forwarder/Release/nvngx.dll_optimizerfps.dll`,
`out/build/x64/core/Release/optimizer-fps-dlss5-core.dll`, and the 23 DXBC files
under `out/build/x64/shaders/`. Stage the DXBC in `optimizer-fps-dlss5/`
beside the core DLL. This count describes the current build; packaging takes
its file list from the build outputs and writes it to `payload/files.sha256`.
The x86 remote add-on is in
`out/build/x86-remote/hosts/remote32/Release/` and runs beside the x86 ReShade
DLL; the x64 payload runs in `host64/`.

For offline bench12 runs, build `tools/bench12/build.cmd`, stage the local runtimes
with `tools/bench12/deploy_addon.ps1` and `deploy_nrhost.ps1`, then use
`tools/bench12/reference_dumps.ps1 -Runtime run_addon -Out candidate_p4t7`
(likewise `run_r521` and `run_nrhost`). Core cases use `-Runtime run_nrhost
-Case core_warp,core_off,core_warp_t1 -Out candidate_p4t7/core`. Compare all
cases with `python tools/bench12/compare_plan2.py --candidate candidate_p4t7`.
These scripts write only to bench runtimes, not to games. Active ReShade bench
settings use `[OptimizerFPS]`; historical reference captures retain their
original `[PeripheralWarp]` section. The `--host core` cases supply settings
through CLI and do not load ReShade.
