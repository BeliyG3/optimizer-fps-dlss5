# Common DLSSNR stage

Optimizer FPS belongs at one shared boundary in a Neural Rendering loader:

```text
game-specific input discovery (Luma / Feeder / OptiScaler input hook)
  -> typed color + depth + motion vectors + metadata
  -> fused Global scale + Optimizer FPS Pack
  -> DLSS Neural Rendering at the work extent
  -> Optimizer FPS Unpack to the native extent
  -> frame generation / present
```

## What is shared

The Pack, Neural Rendering dispatch extent, Unpack, layout validation, fallback, and diagnostic-outline
logic are loader-wide. They must not contain game names, executable hashes, or per-title resource
selection. A loader integrates the stage once and all games that provide a valid DLSS-style input use
the same implementation.

## What remains outside

Optimizer FPS does not discover hidden game buffers. Luma, a Feeder, OptiScaler, or another input
provider must first identify the correct resources and supply the ABI-v2 typed views, subrects, motion
scale/direction, depth convention, jitter semantics, and color encoding.

ABI v3 adds the common spatial layout: raw Work, one X/Y Global scale, effective work dimensions,
and aggressive-compression diagnostics. It does not add another image pass.

The NVIDIA `nvngx_dlssnr.dll` model remains unchanged. It receives only the packed resources and work
dimensions. The reconstructed native image is produced before frame generation or presentation.

For ABI v3 the common stage computes the feature extent as
`even(native * GlobalScale * rawWork)` on each axis. On a compatible D3D12 target the stage resolves
directly into packed color and unpacks directly into the native UAV. Otherwise it uses the generic
adapter and its two copies. This capability decision belongs to the shared loader path and must not
be duplicated per game.

The fastest form of the stage fuses the passes around the model into two compute dispatches:

```text
typed native color + depth + motion
  -> PackEncode      (SDK PwPackTexel + the loader's encode)   -> proxy, packed depth, packed motion
  -> DLSS Neural Rendering at the work extent
  -> ResolveUnpack   (the loader's resolve + SDK unpack mapping) -> the native target, in place
```

It requires typed UAV stores for the packed formats and, for the in-place read of the untouched
frame, typed UAV loads for the native view format; otherwise it writes and samples a packed HDR copy,
and without typed stores at all the loader selects the adapter paths above. The packed data is the
SDK's own (`peripheral_warp_pack.hlsli`), so the model sees the same input on every path.

## Failure rule

If a resource description is missing or unsupported, skip Warp for that signature and run the known
native-resolution NR path. Never present a packed image and never guess a typed view or motion-vector
encoding.

If Pack, Resolve, or Unpack fails after a layout was accepted, abort that Warp evaluation and use the
known native-resolution NR path. Never copy a previous frame's packed or resolved resource as a
recovery shortcut.
