# Continuous integration

`.github/workflows/build.yml` builds five presets on GitHub-hosted
`windows-2022` and uploads the shippable binaries. It runs on every push and pull request
to any branch, and on demand (`workflow_dispatch`). Runs are grouped per ref and a new push
cancels the previous one.

## What the job does

| # | Step | Command |
|---|------|---------|
| 1 | ABI, core include, file-size and PowerShell 5.1 guards | `python tools/check-abi-freeze.py`, `python tools/check-core-includes.py`, `python tools/check-file-size.py --changed`, explicit installer paths plus base/HEAD diff, `tools\Lint-PowerShell51.ps1 -Path tools` |
| 2 | Restore `external\` from the actions cache | `actions/cache@v4` |
| 3 | Configure + build five presets | `cmake --preset <preset>` / `cmake --build --preset <preset>-release --parallel`; presets: `windows-x64`, `windows-x64-mt`, `windows-x86`, `windows-x86-remote`, `sdk-only-x64` |
| 4 | Inventory + run three CTest suites | `ctest --preset <preset>-release -N` / `ctest --preset <preset>-release --output-on-failure`; x64/x86/SDK-only x64: 26/13/10 tests |
| 5 | Verify fetched dependencies | `tools\Verify-Dependencies.ps1` |
| 6 | Package and validate isolated zip | `tools\Package-Release.ps1 -Out <temp> -Force` / `tools\Test-ReleasePackage.ps1 -Zip <temp>.zip` |
| 7 | Run installer self-tests with x64/x86 PE fixtures | `tools\installer-tests\Run-InstallerTests.ps1 -Payload <stage>\payload -Zip <zip> -ReShade64 <x64 stub> -ReShade32 <x86 stub> -OldAddon64 <x64 add-on>` |
| 8 | Stage and upload artifacts | `tools\Runtime-Payload.ps1` / `actions/upload-artifact@v4` |

The release workflow runs the same gate before hashing, upload and publish. Checkout uses
`fetch-depth: 0`; CI checks the installer paths explicitly and changed code paths from
`git diff --name-only`, since `--changed` sees nothing in a clean checkout. The x64
build add-on is a compatibility fixture for `OldAddon64`, not a historical binary.
Any nonzero guard, configure, build, CTest, dependency, package, or installer
self-test result fails the job. A missing artifact fails upload. CTest skip 77
for the image comparison means its Python image dependencies are absent; it is
not evidence that the comparison passed. A package zip is not published until
the release workflow's checks pass.

### No developer command prompt

The presets use `OFPS_*` options and enable `/WX` (including `fxc /WX`). The x64 add-on dynamically loads
`ofps_core_dll`; static `ofps_core` remains for tests. Win32 has the SDK and no
frame core DLL. `ofps_core_includes` checks the core dependency boundary. The
`ofps_compare_reference` test returns SKIP 77 only when NumPy/Pillow are unavailable;
install both to exercise comparison tests rather than accepting that skip.

The workflow never calls `vcvars*.bat`. The `Visual Studio 17 2022` generator locates the
MSVC toolchain by itself, and `cmake/CompileShaders.cmake` finds `fxc.exe` by globbing
`C:/Program Files (x86)/Windows Kits/10/bin/*` (verified: a local cache holds
`OFPS_FXC_EXECUTABLE=.../Windows Kits/10/bin/10.0.26100.0/x64/fxc.exe`, resolved without a
developer prompt). `dxc.exe` is only looked for under `$env:VULKAN_SDK`, which the runner does
not have, so CI produces DXBC but no SPIR-V artifacts. That is expected.

### The WARP smoke tests

`ofps_sdk_d3d11_smoke` and `ofps_sdk_d3d12_smoke` create a WARP device and are
expected to pass headless on the runner. They are **not** excluded. If a future runner image
breaks them, gate only those two rather than dropping the whole step:

```
ctest --preset windows-x64-release --output-on-failure -E "d3d1[12]_smoke"
```

and open an issue, so the remaining tests keep failing loudly.

The 26-test x64 suite includes `ofps_core_api` and `ofps_core_api_dll`,
`ofps_core_loader` (no WARP required), and `ofps_direct_host` (event priority and latch).
It also exercises resource pools, model-host translation and host
shapes with WARP and fake model hosts. No registered test is currently excluded by the
workflow. These tests do not run NVIDIA NR, optical flow, real host codecs, background
NR scheduling or game integration. Those hardware/runtime scenarios are outside WARP
coverage and require the local benches; 26 passing tests are not a substitute for them.

## Caching

`cmake/Dependencies.cmake` sets `FETCHCONTENT_BASE_DIR` to `external/`, so the ReShade,
Dear ImGui and Detours tarballs, their unpacked `*-src` trees and the FetchContent stamp files
all live under `external/`. The whole directory is cached with the key

```
pw-external-v1-Windows-<hash of DEPENDENCIES.lock.json + cmake/Dependencies.cmake>
```

Change either file and the cache key changes, so the dependencies are re-fetched and re-verified.
To force a fresh fetch without touching those files, bump the `pw-external-v1-` prefix in the
workflow. Nothing under `out/` is cached: the builds are always from scratch.

## Artifacts

Uploaded as a single artifact named `optimizer-fps-dlss5-windows` (`if-no-files-found: error`, so a
renamed output breaks the build instead of silently shipping nothing):

- `out/artifact/x64/optimizer-fps-dlss5.addon64` - the 64-bit shell, static CRT
- `out/artifact/x64/optimizer-fps-dlss5-core.dll` - shared core, static CRT
- `out/artifact/x64/nvngx.dll_optimizerfps.dll` - the NGX forwarder
- `out/artifact/x64/optimizer-fps-dlss5/*.dxbc` - all built shaders beside the core (23 in the current build)
- `out/artifact/x86/optimizer-fps-dlss5-remote.addon32` - the 32-bit remote overlay

`ofps_shaders` builds the DXBC delivery names from `sdk/shaders/` and
`core/shaders/`. SDK install sources now come from `sdk/`; the package name is
`OptimizerFpsSdk`, with `OptimizerFps::Sdk*` targets. `Runtime-Payload.ps1` stages
the DLL and its shaders together from the x64-mt build; the ZIP has the same payload.

## Running the same steps locally

From the repository root, run Python and PowerShell checks directly. On the
maintainer's machine, run CMake and CTest through the Visual Studio 2022 tools:

```powershell
python tools/check-abi-freeze.py
python tools/check-core-includes.py
python tools/check-file-size.py --changed
python tools/check-file-size.py tools/installer tools/installer-tests tools/Install-OptimizerFPS.ps1 tools/Verify-OptimizerFPS.ps1
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/Lint-PowerShell51.ps1 -Path tools
foreach ($preset in @('windows-x64','windows-x64-mt','windows-x86','windows-x86-remote','sdk-only-x64')) {
    $configure = 'call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" && "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --preset ' + $preset
    cmd /c $configure
    if ($LASTEXITCODE -ne 0) { throw "Configure failed: $preset" }
    $build = 'call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" && "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --build --preset ' + $preset + '-release --parallel'
    cmd /c $build
    if ($LASTEXITCODE -ne 0) { throw "Build failed: $preset" }
}
foreach ($preset in @('windows-x64','windows-x86','sdk-only-x64')) {
    $ctest = 'call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" && "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe" --preset ' + $preset + '-release'
    cmd /c ($ctest + ' -N')
    if ($LASTEXITCODE -ne 0) { throw "CTest inventory failed: $preset" }
    cmd /c ($ctest + ' --output-on-failure')
    if ($LASTEXITCODE -ne 0) { throw "CTest failed: $preset" }
}
.\tools\Verify-Dependencies.ps1
$out = Join-Path ([IO.Path]::GetTempPath()) ('ofps-ci-' + [Guid]::NewGuid().ToString('N'))
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/Package-Release.ps1 -BuildDirX64 out/build/x64-mt -BuildDirX86 out/build/x86-remote -Out $out -Force
$version = ([regex]::Match((Get-Content cmake/Version.cmake -Raw), 'OFPS_RELEASE_VERSION\s+"([^"]+)"')).Groups[1].Value
$stage = Join-Path $out ('Optimizer-FPS-for-DLSS5-' + $version)
$zip = Join-Path $out ('Optimizer-FPS-for-DLSS5-' + $version + '.zip')
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/Test-ReleasePackage.ps1 -Zip $zip
$x64 = @(Get-ChildItem out/build/x64/tests -Recurse -File -Filter reshade-installer-stub.dll)
$x86 = @(Get-ChildItem out/build/x86/tests -Recurse -File -Filter reshade-installer-stub.dll)
if ($x64.Count -ne 1 -or $x86.Count -ne 1) { throw 'Expected one PE stub per architecture' }
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/installer-tests/Run-InstallerTests.ps1 -Payload (Join-Path $stage 'payload') -Zip $zip -ReShade64 $x64[0].FullName -ReShade32 $x86[0].FullName -OldAddon64 out/build/x64/hosts/reshade/Release/optimizer-fps-dlss5.addon64
```

The workflows check `Get-PeInfo.Bits` (64/32) and both ReShade add-on
exports on the fixture DLLs before the self-test. The PE stubs exercise file
checks only; local acceptance also runs with real ReShade DLLs from bench12
and bench9 copies. Neither workflow launches a game.

## Lint-PowerShell51

`tools\Lint-PowerShell51.ps1 -Path <files or dirs>` walks every `*.ps1`/`*.psm1` and reports,
with file and line number:

- parse errors from `[System.Management.Automation.Language.Parser]::ParseFile`;
- `[IO.Path]::GetRelativePath`, `[Convert]::ToHexString`, `ConvertFrom-Json -AsHashtable`,
  `??` / `??=`, `$x?.Member` / `$x?[0]`, `Join-Path` with 3+ positional arguments,
  and `#Requires -Version 6+`;
- `PSUseCompatibleSyntax` against 5.1 when PSScriptAnalyzer is installed - otherwise it prints
  a notice and continues (the runner image is not guaranteed to ship the module for Windows
  PowerShell 5.1; add an `Install-Module PSScriptAnalyzer -Scope CurrentUser -Force` step to the
  workflow if that check should be mandatory).

Exit code is 1 on any finding. Comments and string bodies are blanked before the pattern rules
run, so documentation and regex literals do not produce false positives; the `Join-Path` and
ternary checks are deliberately heuristic (ternaries are not checked at all - under 5.1 they are
a parse error, which the parse check already catches).

Run it under `powershell.exe` (Windows PowerShell 5.1), not `pwsh`, so the parse check uses the
5.1 parser - that is what the workflow does. To pass several paths from the command line use
`-Command`, not `-File`; `powershell.exe -File` hands a comma-separated list over as one string:

```powershell
powershell.exe -NoProfile -Command "& '.\tools\Lint-PowerShell51.ps1' -Path tools"
```

Both workflows lint the whole `tools` directory: every script a user is expected to run on a
stock Windows box has to parse and run under Windows PowerShell 5.1.

## Local and GitHub coverage

The workflow YAML and `run:` blocks can be checked locally. The commands above
match the workflow's build, test, package, and installer checks. A local pass
does not claim a GitHub run. This documentation update does not trigger either
workflow; publishing is a separate release step.

Local regression uses `reference_before_2026.9.1` captures. D3D12 NR parity and the
Feeder/bench9 smoke require separately installed runtimes and a supported GPU. D3D11
NR acceptance is BLOCKED when `dlss5-dx11-bridge.addon64` is absent. These are not CI jobs.

## Release workflow

`.github/workflows/release.yml` runs on a `v*` tag: checks the tag against `OFPS_RELEASE_VERSION`, runs the same five-build/three-CTest gate, packages and validates the zip in runner temp with `tools/Package-Release.ps1`, then copies the checked zip to `dist-release/`, extracts the CHANGELOG section and publishes a GitHub Release. Checklist: `docs/RELEASING.md`.

Packaging reads the x64-mt and x86-remote build trees, including the compiled shader
outputs, and preserves the add-on/forwarder/remote filenames. `dist-release/` and all
local bench runtimes/captures are ignored by Git.
