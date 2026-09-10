# Build

Requirements on Windows:

- Visual Studio 2022 with MSVC and Windows SDK;
- CMake 3.25 or newer (the presets require it, and `FetchContent` needs `DOWNLOAD_EXTRACT_TIMESTAMP`);
- FXC for DXBC (part of the Windows SDK);
- DXC with SPIR-V support (optional, from the Vulkan SDK);
- an internet connection the first time the ReShade add-on is configured, or local copies of its dependencies.

## Dependencies

`cmake/Dependencies.cmake` fetches the three header/source dependencies of the ReShade add-on and
pins every archive by SHA-256:

| Dependency | Pin | Used for |
| --- | --- | --- |
| ReShade add-on SDK | tag `v6.8.0` | `include/` only |
| Dear ImGui | tag `v1.92.5-docking` | headers only; `reshade_overlay.hpp` needs the **docking** branch (`ImGuiDockNodeFlags`) and checks `IMGUI_VERSION_NUM == 19250` |
| Microsoft Detours | commit `adb07604aa56508448b95bf037c2a6d0d3b6831a` | five translation units compiled into the static `pw_detours` |

They land in `external/` (`external/reshade-src`, `external/imgui-src`, `external/detours-src`), which
is git-ignored. Point `FETCHCONTENT_BASE_DIR` somewhere else to share one download between clones.

Nothing is downloaded when you supply your own trees:

- `PW_RESHADE_SDK_ROOT` - directory containing `include/reshade.hpp`;
- `PW_IMGUI_ROOT` - directory containing `imgui.h` (docking branch, 1.92.5);
- `PW_DETOURS_ROOT` - a **prebuilt** Detours tree with `include/detours/detours.h` and
  `library/detours/detours.lib` (e.g. the OptiScaler vendored copy); nothing is compiled then.

Each is an empty cache entry by default; set it to a non-empty path to take over.

## Presets

```powershell
cmake --preset windows-x64
cmake --build --preset windows-x64-release
ctest --preset windows-x64-release
```

| Configure preset | Build dir | What it produces |
| --- | --- | --- |
| `windows-x64` | `out/build/x64` | everything: core, D3D11/D3D12 adapters, shaders, tests, `optimizer-fps-dlss5.addon64`, `nvngx.dll_optimizerfps.dll` |
| `windows-x64-mt` | `out/build/x64-mt` | the same add-on with `PW_STATIC_CRT=ON` (`/MT`): the game process pulls in no `MSVCP140`/`VCRUNTIME140` |
| `windows-x86` | `out/build/x86` | 32-bit core, adapters and tests |
| `windows-x86-remote` | `out/build/x86-remote` | only `optimizer-fps-dlss5-remote.addon32` (ReShade + ImGui headers, no core, no Detours) |
| `core-only-x64` | `out/build/core-x64` | `peripheral_warp_core` and the tests, no adapters/shaders/add-on |

Matching build presets are `<name>-release`; test presets exist for `windows-x64`, `windows-x86` and
`core-only-x64`. The `windows-x86-remote` build preset builds just the
`peripheral_warp_reshade_remote` target.

Ready-made wrappers live in `tools/bench`: `build_sdk_addon.cmd` (x64 + ctest),
`build_addon_mt.cmd` (static CRT, prints the DLL dependents) and `build_remote32.cmd`.

Artifacts are emitted below `out/build/<preset>`:

- `peripheral_warp_core.lib`;
- `peripheral_warp_d3d11.lib`;
- `peripheral_warp_d3d12.lib`;
- `adapters/reshade/Release/optimizer-fps-dlss5.addon32|64`;
- `adapters/reshade/Release/optimizer-fps-dlss5-remote.addon32` (remote preset);
- `adapters/reshade/ngx_forwarder/Release/nvngx.dll_optimizerfps.dll`;
- `shaders/*.dxbc` and `shaders/*.spv`.

## Shaders

`PW_COMPILE_SHADERS=ON` (default on Windows) compiles the DXBC set with `fxc.exe`, which is looked up
in the installed Windows SDKs, and the optional SPIR-V set with `dxc.exe`, looked up in
`$env:VULKAN_SDK\Bin` and on `PATH`. Both can be pointed at explicitly:

```powershell
cmake --preset windows-x64 `
  -DPW_FXC_EXECUTABLE="C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\fxc.exe" `
  -DPW_DXC_EXECUTABLE="C:\VulkanSDK\1.4.309.0\Bin\dxc.exe"
```

A missing `fxc.exe` is a hard error; a missing `dxc.exe` only reports a status message and skips the
SPIR-V artefacts.

The compiled shader set contains `fullscreen_vs`, `pack_ps`, `unpack_ps`, `preview_ps`, `outline_ps`
and the `temporal_*` passes. The outline shader is optional for API consumers, but required for the
session-only **Show uncompressed center** control.

## Dependency lock

The exact dependency files used for the published Candidate are recorded in `DEPENDENCIES.lock.json`
(archive URLs and their SHA-256, plus per-file hashes). Do not substitute an arbitrary
ReShade/ImGui pair: ReShade's add-on function table checks the ImGui ABI version at load time, and
`cmake/Dependencies.cmake` refuses a mismatching `IMGUI_VERSION_NUM`.

Verify the fetched trees, or a hand-provided pair, against the lock:

```powershell
.\tools\Verify-Dependencies.ps1
.\tools\Verify-Dependencies.ps1 -ReShadeRoot C:\src\reshade -ImGuiRoot C:\src\imgui -DetoursRoot C:\src\Detours
```

With no parameters it checks `external\reshade-src`, `external\imgui-src` and `external\detours-src`.
File hashes are taken over CRLF-normalized content, so a git checkout and a GitHub source tarball
verify alike.

## Installing

After `cmake --install`, consumers may use `find_package(PeripheralWarp CONFIG REQUIRED)` and link
`PeripheralWarp::Core`, `PeripheralWarp::D3D11`, or `PeripheralWarp::D3D12` when that adapter was
included in the installed build.

Every configuration installs the public headers, the HLSL sources, the
dependency lock and the complete ReShade / Dear ImGui / Microsoft Detours license texts. The
architecture-specific `optimizer-fps-dlss5.addon32|64` is added only when the add-on was built
(`PW_BUILD_RESHADE_ADDON=ON`); compiled shader binaries are added only when `PW_COMPILE_SHADERS=ON`.
