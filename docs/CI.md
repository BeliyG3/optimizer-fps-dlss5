# Continuous integration

`.github/workflows/build.yml` builds every Optimizer FPS configuration on GitHub-hosted
`windows-2022` and uploads the shippable binaries. It runs on every push and pull request
to any branch, and on demand (`workflow_dispatch`). Runs are grouped per ref and a new push
cancels the previous one.

## What the job does

| # | Step | Command |
|---|------|---------|
| 1 | Lint the scripts for Windows PowerShell 5.1 compatibility | `tools\Lint-PowerShell51.ps1 -Path tools` |
| 2 | Restore `external\` from the actions cache | `actions/cache@v4` |
| 3 | Configure + build the full x64 build | `cmake --preset windows-x64` / `cmake --build --preset windows-x64-release --parallel` |
| 4 | Run the test suite (7 tests) | `ctest --preset windows-x64-release --output-on-failure` |
| 5 | Configure + build the shipping static-CRT build | `cmake --preset windows-x64-mt` / `cmake --build --preset windows-x64-mt-release --parallel` |
| 6 | Configure + build the 32-bit remote overlay add-on | `cmake --preset windows-x86-remote` / `cmake --build --preset windows-x86-remote-release --parallel` |
| 7 | Configure + build the Win32 core/adapters/shaders build and test it | `cmake --preset windows-x86` / `cmake --build --preset windows-x86-release --parallel` / `ctest --preset windows-x86-release --output-on-failure` |
| 8 | Verify the fetched dependency trees against the lock file | `tools\Verify-Dependencies.ps1` |
| 9 | Upload artifacts | `actions/upload-artifact@v4` |

### No developer command prompt

The workflow never calls `vcvars*.bat`. The `Visual Studio 17 2022` generator locates the
MSVC toolchain by itself, and `cmake/CompileShaders.cmake` finds `fxc.exe` by globbing
`C:/Program Files (x86)/Windows Kits/10/bin/*` (verified: a local cache holds
`PW_FXC_EXECUTABLE=.../Windows Kits/10/bin/10.0.26100.0/x64/fxc.exe`, resolved without a
developer prompt). `dxc.exe` is only looked for under `$env:VULKAN_SDK`, which the runner does
not have, so CI produces DXBC but no SPIR-V artifacts. That is expected.

### The WARP smoke tests

`peripheral_warp_d3d11_smoke` and `peripheral_warp_d3d12_smoke` create a WARP device and are
expected to pass headless on the runner. They are **not** excluded. If a future runner image
breaks them, gate only those two rather than dropping the whole step:

```
ctest --preset windows-x64-release --output-on-failure -E "d3d1[12]_smoke"
```

and open an issue, so the five CPU tests keep failing loudly.

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

- `out/build/x64-mt/adapters/reshade/Release/optimizer-fps-dlss5.addon64` - the 64-bit ReShade add-on, static CRT
- `out/build/x64-mt/adapters/reshade/ngx_forwarder/Release/nvngx.dll_optimizerfps.dll` - the NGX forwarder
- `out/build/x64-mt/shaders/*.dxbc` - the compiled shader blobs
- `out/build/x86-remote/adapters/reshade/Release/optimizer-fps-dlss5-remote.addon32` - the 32-bit remote overlay add-on

## Running the same steps locally

From the repository root, in any shell that has CMake on `PATH` (no developer prompt needed):

```powershell
cmake --preset windows-x64
cmake --build --preset windows-x64-release --parallel
ctest --preset windows-x64-release --output-on-failure

cmake --preset windows-x64-mt
cmake --build --preset windows-x64-mt-release --parallel

cmake --preset windows-x86-remote
cmake --build --preset windows-x86-remote-release --parallel

cmake --preset windows-x86
cmake --build --preset windows-x86-release --parallel
ctest --preset windows-x86-release --output-on-failure

.\tools\Verify-Dependencies.ps1
powershell.exe -NoProfile -File .\tools\Lint-PowerShell51.ps1 -Path tools
```

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

## Not verified here

These files were written and checked locally (YAML parsed with PyYAML, the linter exercised
under Windows PowerShell 5.1 against both clean and deliberately broken fixtures), but the
workflow itself has never been executed on GitHub Actions - the first push that reaches a
GitHub remote is also the first real run.

## Release workflow

`.github/workflows/release.yml` runs on a `v*` tag: checks the tag against `PW_RELEASE_VERSION`, builds and tests, packages the zip with `tools/Package-Release.ps1`, extracts the CHANGELOG section and publishes a GitHub Release. Checklist: `docs/RELEASING.md`.
