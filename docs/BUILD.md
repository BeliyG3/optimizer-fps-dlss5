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
| Microsoft Detours | commit `adb07604aa56508448b95bf037c2a6d0d3b6831a` | five translation units compiled into the static `ofps_detours` |

They land in `external/` (`external/reshade-src`, `external/imgui-src`, `external/detours-src`), which
is git-ignored. Point `FETCHCONTENT_BASE_DIR` somewhere else to share one download between clones.

Nothing is downloaded when you supply your own trees:

- `OFPS_RESHADE_SDK_ROOT` - directory containing `include/reshade.hpp`;
- `OFPS_IMGUI_ROOT` - directory containing `imgui.h` (docking branch, 1.92.5);
- `OFPS_DETOURS_ROOT` - a **prebuilt** Detours tree with `include/detours/detours.h` and
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
| `windows-x64` | `out/build/x64` | static core tests, core DLL, D3D11/D3D12 adapters, shaders, `optimizer-fps-dlss5.addon64`, `nvngx.dll_optimizerfps.dll` |
| `windows-x64-mt` | `out/build/x64-mt` | the same add-on with `OFPS_STATIC_CRT=ON` (`/MT`): the game process pulls in no `MSVCP140`/`VCRUNTIME140` |
| `windows-x86` | `out/build/x86` | 32-bit SDK, adapters, shaders and 13 tests; no `ofps_core` |
| `windows-x86-remote` | `out/build/x86-remote` | only `optimizer-fps-dlss5-remote.addon32` (ReShade + ImGui headers, no core, no Detours) |
| `sdk-only-x64` | `out/build/sdk-x64` | SDK only, 10 tests; no core, add-on, D3D adapters or compiled shaders |

The full x64 preset registers 26 tests; x86 registers 13, and SDK-only x64 registers 10. The shipping x64-mt and
x86-remote presets disable tests. All five presets belong to the acceptance matrix.
`ofps_core` is Windows x64-only and independent of ReShade; it requires
`OptimizerFps::SdkD3D12`. `OFPS_BUILD_CORE=OFF` makes SDK-only configuration independent
of that adapter. Neither x86 preset builds the core DLL.

`ofps_core_dll` and static `ofps_core` use the same core sources. The DLL always uses
`/MT` (`/MTd` in Debug), including private SDK copies built by `core/dll.cmake`;
it never links the ordinary `/MD` SDK archives. The shell compiles `CoreLoader` and
loads the DLL dynamically. Static and DLL ABI suites exercise the same ABI 1 header.
The DLL exports exactly `OfpsCreateCore` and `OfpsCoreVersion`.

Windows presets enable `CMAKE_COMPILE_WARNING_AS_ERROR` (`/WX` with MSVC).

Matching build presets are `<name>-release`; test presets exist for `windows-x64`, `windows-x86` and
`sdk-only-x64`. The `windows-x86-remote` build preset builds just the
`peripheral_warp_reshade_remote` target.

Ready-made wrappers live in `tools/bench`: `build_sdk_addon.cmd` (x64 + ctest),
`build_addon_mt.cmd` (static CRT, prints the DLL dependents) and `build_remote32.cmd`.

Artifacts are emitted below `out/build/<preset>`:

- `sdk/Release/ofps_sdk_core.lib`;
- `core/Release/ofps_core.lib` (Windows x64, static tests);
- `core/Release/optimizer-fps-dlss5-core.dll` (Windows x64 runtime);
- `sdk/adapters/d3d11/Release/ofps_sdk_d3d11.lib`;
- `sdk/adapters/d3d12/Release/ofps_sdk_d3d12.lib`;
- `hosts/reshade/Release/optimizer-fps-dlss5.addon64`;
- `hosts/remote32/Release/optimizer-fps-dlss5-remote.addon32` (remote preset);
- `hosts/reshade/ngx_forwarder/Release/nvngx.dll_optimizerfps.dll`;
- `shaders/*.dxbc` and `shaders/*.spv`.

## Shaders

`OFPS_COMPILE_SHADERS=ON` (default on Windows) compiles the DXBC set with `fxc.exe`, which is looked up
in the installed Windows SDKs, and the optional SPIR-V set with `dxc.exe`, looked up in
`$env:VULKAN_SDK\Bin` and on `PATH`. Both can be pointed at explicitly:

```powershell
cmake --preset windows-x64 `
  -DOFPS_FXC_EXECUTABLE="C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\fxc.exe" `
  -DOFPS_DXC_EXECUTABLE="C:\VulkanSDK\1.4.309.0\Bin\dxc.exe"
```

A missing `fxc.exe` is a hard error; a missing `dxc.exe` only reports a status message and skips the
SPIR-V artefacts.

The current `ofps_shaders` target compiles 23 DXBC files from `sdk/shaders/`
and `core/shaders/`. The release tools read the built shader list and package
manifest instead of relying on this number.
`OFPS_COMPILED_SHADER_OUTPUTS` carries the generated outputs; both compiler include paths
cover the two source directories. `core/shaders/temporal_passes.def` is the temporal manifest.
The outline shader is optional for API consumers, but required for the
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

After `cmake --install`, consumers may use `find_package(OptimizerFpsSdk CONFIG REQUIRED)` and link
`OptimizerFps::SdkCore`, `OptimizerFps::SdkD3D11`, or `OptimizerFps::SdkD3D12` when that adapter was
included in the installed build.

For example, `cmake --install out/build/x64 --config Release` installs SDK 0.6.0 under
`out/install/x64`: headers in `include/optimizer_fps/`, adapter headers in
`include/optimizer_fps/adapters/{common,d3d11,d3d12}/`, libraries in `lib/`, package files
in `lib/cmake/OptimizerFpsSdk/`, and shader sources from both source trees in
`share/optimizer-fps-sdk/shaders/`.
The static `ofps_core` / `OptimizerFps::Core` target is consumed inside the build tree
through `core/api/ofps_core.h` (ABI 1); it is not exported by the installed SDK package.
The add-on dynamically loads `optimizer-fps-dlss5-core.dll` beside its host module.
Deploy all built DXBC in `optimizer-fps-dlss5/` beside that DLL, independent of the
executable directory or current working directory. The SDK export/install API is unchanged.

Every configuration installs the public headers, the HLSL sources, the
dependency lock and the complete ReShade / Dear ImGui / Microsoft Detours license texts. The
architecture-specific `optimizer-fps-dlss5.addon32|64` is added only when the add-on was built
(`OFPS_BUILD_RESHADE_ADDON=ON`); compiled shader binaries are added only when `OFPS_COMPILE_SHADERS=ON`.
