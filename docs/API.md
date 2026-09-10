# Public API

This is the API of the Optimizer FPS SDK — the layout maths, the D3D11/D3D12 adapters and the
shaders. It says nothing about the ReShade add-on, which links the SDK statically and exports none of
it; for that see [RESHADE_ADDON.md](RESHADE_ADDON.md).

ABI versions `1` and `2` remain binary-compatible. Version `3` is a separate opt-in export for spatial scaling. Cross-process descriptors are pointer-free. `structSize`, `version`, reserved fields, flags, dimensions, formats, and layouts are validated rather than silently corrected.

## Versions

Two independent version sources live in `cmake/Version.cmake`:

- `PW_SDK_VERSION` - the semantic version of this SDK. It is the CMake `project()` version, the
  installed package version and what `find_package(PeripheralWarp <ver>)` matches. It moves only
  when the library or its public API/ABI changes. The ABI version numbers above (`1`, `2`, `3`) are
  separate and are not derived from it.
- `PW_RELEASE_VERSION` - the user-facing release number of the ReShade add-on (the 26.x line the
  changelog is written in). It moves with every shipped add-on build and never affects the SDK.

CMake generates `pw_version.h` from `cmake/pw_version.h.in` into the build tree with
`PW_SDK_VERSION_STRING` and `PW_ADDON_VERSION_STRING`, and regenerates the checked-in `NOTICE` from
`NOTICE.in`. The add-on's exported ReShade `NAME` deliberately carries no version - ReShade keys its
`DisabledAddons` list on `NAME` - so the release number is reported in `DESCRIPTION` and in the
overlay banner instead. Configuring fails if `CHANGELOG.md` has no `## <PW_RELEASE_VERSION>` section.

## Core types

Include:

```cpp
#include <peripheral_warp/types.h>
#include <peripheral_warp/math.h>
```

`pw::ConfigV1` selects `Off`, `Uniform`, or `Peripheral`, independent X/Y axis values, the color filter, and flags. Use `pw::DefaultConfigV1()`, change explicit fields, then call `pw::ValidateConfig()`.

`pw::BuildLayout(config, nativeWidth, nativeHeight, &layout)` computes exact even work dimensions and all shader coefficients. Consumers receiving a serialized or foreign layout must call `pw::ValidateLayout()`.

Coordinates are continuous pixel-center coordinates; the first texel center is `0.5`. Motion is always `current pixel -> previous pixel`, measured in pixels.

```cpp
pw::Float2 packedMotion = pw::PackMotion(nativePosition, nativeMotion, layout);
pw::Float2 nativeMotion = pw::UnpackMotion(packedPosition, packedMotion, layout);
```

Do not replace endpoint mapping with a global resolution multiplier. The local scale varies across the periphery.

## Spatial ABI v3

Request `PeripheralWarpGetApiV3(3)` when the consumer needs Global scale or a raw Work value below the legacy guard. `ConfigV2` retains the axis mapping and adds one `globalScalePercent` shared by X and Y. `LayoutV2` reports raw Work fractions/dimensions separately from the effective NR dimensions.

The effective mapping is mathematically `GlobalScale * Pack(native)`, but adapters execute it in the same Pack pass. Center remains free from nonlinear deformation; when Global scale is below 100%, its sampling density is uniformly reduced as well. The per-axis effective extent must remain at least 25% of native.

The renderer extent is derived only after the requested dimensions have been rounded to valid even
integers:

```text
NR width  = even(native width  * globalScale * rawWorkX)
NR height = even(native height * globalScale * rawWorkY)
```

`PwLayoutV2::rawWorkWidth/rawWorkHeight` describe the layout before Global scale;
`workWidth/workHeight` are the effective NR dimensions. Do not use raw Work as the feature extent.

`aggressiveCompression` is diagnostic rather than a rejection. Consumers should warn when either axis has `compression < 0.5`, keep the automatic adaptive filter enabled, and continue only with the explicitly requested valid layout.

## ReShade producer ABI (retired)

> **Retired in 26.26 (stage 27.C).** `optimizer-fps-dlss5.addon64` is an NGX interposer only: it no
> longer exports `PeripheralWarpGetApi` / `GetApiV2` / `GetApiV3`, registers no consumers and
> publishes no packed frames, and the `PeripheralWarp.fx` producer sample is gone with it. The
> headers `include/peripheral_warp/addon_api.h`, `addon_api_v2.h` and `addon_api_v3.h` remain in the
> SDK as the ABI of the retired ReShade producer sample, kept for source compatibility; nothing in
> this tree implements them. The section below describes that historical contract.
>
> What a consumer integrates with today is the layout bridge (`layout_bridge_v1.h`) and the
> `PeripheralWarpSetLayoutV1` / `PeripheralWarpSetTemporalV1` exports.

Load the exported function and request the exact ABI:

```cpp
using GetApi = const pw::AddonApiV1 *(PW_CALL *)(std::uint32_t);
auto getApi = reinterpret_cast<GetApi>(GetProcAddress(module, "PeripheralWarpGetApi"));
const pw::AddonApiV1 *api = getApi ? getApi(pw::kAddonApiVersion) : nullptr;
```

`registerConsumer` installs an in-process synchronous callback. The callback must be `noexcept` in practice: exceptions must not cross the C ABI. A callback receives a `PackedFrameV1`; its ReShade objects and CPU views are valid only until the callback returns. Copy commands may be recorded on the supplied command list, but handles must not be retained. Consumer-owned resources referenced by those commands must remain alive until GPU retirement. Restore command-list state and leave all packed inputs in shader-resource usage before returning so the next consumer remains valid.

`submitInput` is the D3D11-only semantic-rebinding path. Call it from the active `reshade_begin_effects` pass with that pass's command list; it binds canonical full-resolution inputs, executes Pack against the pass RTV, restores prior semantic bindings, and publishes synchronously. D3D12 integrations use the native adapter.

`publishPackedFrame` publishes data already produced by a native adapter and performs no GPU work. Its layout and generation must match `getLayout`, and it must be called on the same serialized ReShade render thread during the matching active effects pass. Registration, unregistration, configuration changes, nested publication, and `submitInput` are rejected with `NotReady` during callback dispatch. Do not unregister concurrently with dispatch.

Required input formats:

| Data | Format |
|---|---|
| Color | RGBA8/BGRA8 (linear or sRGB view) or RGBA16F |
| Depth | R32_FLOAT |
| Motion | R16G16_FLOAT, current → previous, pixels |
| Confidence | optional R16_FLOAT |

## Universal ABI v2

Request `PeripheralWarpGetApiV2(2)` and fill `FrameInputV2`. Each resource has its own
typed view format and extent; `InputDescriptionV2` supplies its subrect and semantics.
Typeless allocations are accepted only through an explicit compatible typed view.

`motionScaleX/Y` converts the stored two-channel value directly to native-output pixels.
`motionDirection` then normalizes previous→current inputs to the canonical current→previous
direction. Guide resolution may differ from output resolution. Do not hide that conversion
in a guessed global percentage.

Supported source families are RGBA8/BGRA8, RGB10A2, RG11B10 and RGBA16F color; R16F,
R16_UNORM, R24 and R32 depth views; RG16F, RG32F, RG16_SNORM and RG16_UNORM motion;
and optional R8_UNORM, R16F or R32F confidence. Integer, compressed, planar, MSAA and
non-sampleable views are rejected. The packed result stays R32F depth, RG16F motion in
native-output pixels, and R16F confidence.

Depth values are copied without linearization or inversion. `depthConvention`, jitter values,
the `MotionJittered` bit, and color encoding are forwarded as metadata. The module never guesses them.

Request the v2 entry points in-process from the linked SDK; `PeripheralWarpGetApiV2` is not exported
by any shipped binary (see the retired section above).

The calling module owns queue submission, barriers, synchronization, temporal reset, and source lifetime. Neither the core nor the native adapters add an explicit CPU/GPU wait. The caller remains responsible for any synchronization its own work requires.

## Fast-path contract

The D3D12 integration may bind the packed-color UAV directly as the NR resolve destination and bind
an UAV-compatible native target directly for color-only Unpack. This removes the temporary native
Unpack surface and two full-frame copies. It is optional: an unsupported typed view or UAV capability
must select the generic adapter path, not reinterpret the resource. A failed dispatch must not copy or
present stale packed data.

`D3D12Adapter::Initialize(..., framesInFlight, allocateConfidence, sourceSets)`: the external source
sets (the four SRVs of the host's inputs plus the 256-byte input constants that `WriteSourceDescriptors*`
writes) are CPU-written, so a set the GPU may still be reading must not be rewritten, while the packed
textures of a frame slot can be reused as soon as the queue orders the next draw after the previous
reads. `sourceSets` (0 = `framesInFlight`, the previous one-set-per-slot behaviour) sizes a separate
ring of sets: `WriteSourceSetV2(set, ...)`, `RecordPackFromSet(list, frameSlot, set[, targets])`,
`RecordUnpackColorFromSet(list, frameSlot, set, ...)` and `RecordUnpackOwnedColorFromSet(list,
frameSlot, set, ...)` address a set independently of the texture slot; the slot-addressed methods
are the `set == frameSlot` case. A host that changes its input resources every frame takes a fresh set
per frame from a ring deeper than its GPU queue lag (`SourceSetCount()` reports the size).

`D3D12Adapter::Initialize(..., framesInFlight, allocateConfidence)` lets a color-only NR consumer skip
the packed confidence allocation: the pack shader still exports four targets, but the fourth is a null
RTV and `PackedViews` reports a null confidence resource with `R16_FLOAT`. The adapter rejects
`allocateConfidence == false` together with `ConfigFlagInputConfidenceValid`, and `RecordUnpackOwned`
(the four-target owned Unpack) reports `ResourceMismatch` for such an adapter; use the color-only
Unpack entry points instead. When the typed color view supports UAV stores, the owned packed color is
created with `ALLOW_UNORDERED_ACCESS` so the fast path above can resolve into it directly.

## Fused stage contract

An integrator may skip the adapter entirely and record two compute dispatches of its own:

- **Pack fused with its encode.** Include `peripheral_warp_common.hlsli` and
  `peripheral_warp_pack.hlsli` (override `PW_WARP_CONSTANTS_REGISTER`, `PW_INPUT_CONSTANTS_REGISTER`
  and `PW_DIAGNOSTIC_CONSTANTS_REGISTER` as needed), bind the four inputs at t0-t3 and the SDK's
  linear/point clamp samplers at s0/s1, and call `PwPackTexel(workPixel + 0.5, ...)` per work texel.
  Feed b1 with `BuildShaderConstants(layout)` and b2 with the `ShaderInputConstantsV2` returned by
  `ValidateD3D12Sources`. The packed colour, depth and motion are then byte-identical to `pack_ps`.
- **Unpack fused with its resolve.** Map each native pixel with `PwPackNativePixel(nativePixel + 0.5)`
  and sample the work-size inputs bilinearly at that position; draw the session outlines with
  `PwDiagnosticOutlineColor`. Reading the untouched frame from the native target in place requires
  typed UAV loads for the target's view format; without them keep a packed HDR copy and sample it.

The fused stage must reject exactly what `WriteSourceDescriptorsV2` rejects (use
`ValidateD3D12Sources` for that), must leave its work-size resources in a known state between
frames, and must fall back to the adapter paths when typed UAV stores for the packed formats are
missing. Compare and debug views that read the original off-pixel belong to the separate passes.
