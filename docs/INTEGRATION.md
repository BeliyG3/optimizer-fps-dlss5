# Integration

## Correct pipeline position

Pack all temporal inputs before the expensive renderer and Unpack its result before any native-coordinate consumer:

```text
native color + native depth + native MV
  -> fused Global scale + Pack color/depth/MV/confidence
  -> temporal renderer at layout.workWidth × layout.workHeight
  -> Unpack color/depth/MV/confidence
  -> frame generation / native backbuffer
```

Pack and Unpack on their own do not reduce another mod's workload. The renderer must explicitly consume the packed resources and create its feature at the work extent.

Integrate this sequence once in the loader's common NR call path. Do not duplicate it in individual
game profiles. Per-game code, when unavoidable, ends after it has supplied a valid ABI-v2 input
description. See [Common DLSSNR stage](DLSSNR_COMMON_STAGE.md).

## Resource contract

- Resolve MSAA before Pack.
- Use one matching frame of color, depth, motion, and confidence.
- With ABI v1, provide canonical R32F depth and RG16F current→previous motion in native pixels.
- With ABI v2, provide explicit typed views and metadata; the Pack pass performs normalization.
- Preserve the layout and frame generation until all consumers finish that frame.

For motion, Pack and Unpack both transform the current and previous endpoints:

```text
packedMV = Pack(current + nativeMV) - Pack(current)
nativeMV = Unpack(packedCurrent + packedMV) - Unpack(packedCurrent)
```

After Unpack, pass native pixel motion to a consumer. For Streamline, normalized motion scale is normally `{1/nativeWidth, 1/nativeHeight}`.

## Adapter responsibilities

The adapters own shaders, pipelines, descriptors, constant buffers, and intermediate packed textures where exposed by their API. The caller owns:

- resource-state transitions and render-pass compatibility;
- command-list submission;
- fences/timelines and frame-slot retirement;
- reset of temporal consumers after a layout or resolution change;
- input and output resources.

Every D3D12 `Record*` call replaces the command list's descriptor heap, root signature, pipeline state, root arguments, viewport, scissor, primitive topology, and render targets. D3D12 cannot query and restore that state. Before recording subsequent application work, the caller must explicitly rebind every graphics state that work depends on.

Do not reset or destroy a frame slot until its GPU work has retired.

## Unsupported formats

Treat `UnsupportedFormat`, `ResourceMismatch`, or any validation failure as identity/passthrough. Never present an intermediate packed image directly.

Scope failures to the input signature. A changed typed format, extent, subrect, generation, or
an explicit Retry may be tried again. Until then, keep the established native-resolution renderer path.

Draw the optional Center and raw Work outlines only after Unpack and after temporal rendering. Toggling either must not rebuild resources or reset NR/FG. Do not add a second fullscreen pass for the Work outline.

For an NR-specific D3D12 fast path, Resolve may write directly to the packed color UAV and a color-only Unpack may write directly to the caller's UAV-compatible native target. Keep the generic adapter path as fallback and never expose the packed frame if the direct path is unsupported.

The direct path is selected from the real typed-view capabilities, not the allocation's typeless
format or a game name. Formats such as an incompatible `R10G10B10A2` typeless target remain on the
generic two-copy fallback. The fallback is correct but costs more bandwidth.

Reliable per-stage GPU timings require access to the fence that proves the submitted timestamp
queries have retired. An integration that only records commands into an application-owned command
list must report those timings as unavailable. Existing aggregate NR timing is best-effort and must
not be presented as separate Pack, Resolve, or Unpack durations.
