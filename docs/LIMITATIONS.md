# Limitations

## The warp itself

- Non-linear input density is not a documented DLSS operating mode. It works, but it is not
  something NVIDIA supports.
- Quality may soften near the edges, and can change as an object crosses the centre boundary.
- Hair, particles, transparency, HUD and disocclusions stay sensitive to imperfect motion and depth.
- Work below the legacy 2x-compression guard is intentionally experimental and can alias near the
  screen edge.
- Global scale also reduces sampling density in the undeformed centre; below 100 % it does not
  preserve native centre detail.
- The model's tonal response differs slightly on a compressed frame. The Brightness/Gamma
  compensation is applied before the host decodes the model's output, so it is not 1:1 with the
  final picture and has to be tuned by eye.
- Eye tracking and a moving foveal centre are out of scope for the 0.5 SDK.

## The temporal modes

- Interpolated frames ghost on fast camera turns and around disocclusions, and small detail shimmers
  at the full-pass rate because it only refreshes on a full pass.
- The model's own tone statistics lag by the skipped frames: a full pass after N−1 skipped frames is
  darker than an every-frame result while the scene changes. This is the model, not the vectors.
- Background mode needs a D3D12 queue that ReShade reported. When the NR consumer creates its device
  before ReShade is in place — always the case when OptiScaler loads ReShade — no queue is
  registered and the synchronous interpolation runs instead.
- Background mode trades quality for even frame times when the GPU is saturated: the residual ages
  further the longer a pass takes.

## The add-on

- Windows and D3D12 only. The NGX interposer hooks `nvngx_dlssnr.dll` and records D3D12 work; there
  is no D3D11, Vulkan or OpenGL path through it.
- It accelerates only a renderer that goes through NGX feature 18. It cannot speed up an unrelated
  closed renderer, and it never searches private game buffers or infers missing semantics — a
  consumer must already supply valid colour, depth, motion and their metadata.
- Only one feature is warped at a time. A second NR consumer in the same process is passed through
  until the first one's feature is gone.
- The frame is compressed only when the consumer hands the model a colour region and an output region
  of the feature's size, the output region at the top-left of its texture (larger textures are fine).
  The NR runtime (310.8) does not upscale inside the model: consumers that offer a lower NR working
  resolution (renodx-dlss5 "Scaled" / "Follow render resolution"; by its logs, the RenoDX DLSS
  build as well) run the model on the reduced frame and upscale it themselves, and that reduced
  frame is what gets compressed. A consumer whose colour region changes size every frame, or whose output region is
  elsewhere in its texture, runs the model untouched, and the temporal modes are off for it.
- Per-stage GPU timing is limited. `DebugTiming` measures the model's evaluate with timestamp
  queries; it is not a Pack/Unpack profiler.
- 32-bit games are supported only through DLSS5-Feeder's 64-bit host process, and the in-game tab
  there is a remote view of that process.
- The add-on cannot tell a frame another tool has already compressed from a normal one. With an
  OptiScaler build that has its own peripheral compression (wilsjo2 `SpatialCompression`), use one
  of the two, not both.
- The add-on does not generate frames. Frame generation comes from the consumer's side: OptiScaler's
  own `[FrameGen]`, DLSS5-Feeder's presenter for 32-bit games, or the game's own DLSS-G.
- Unloading the add-on while the game runs is not supported: the hooks stay until the process exits.
  Change or update it with the game closed.

## The SDK

- Runtime shader compilation is intentionally absent: consumers package compiled blobs. SPIR-V
  artefacts are produced by the build, but there is no Vulkan adapter — the shipped adapters are
  D3D11 and D3D12, and they are Windows-only.
- The direct-UAV fast path depends on typed-view and UAV capabilities. Unsupported targets fall back
  to the correct but more bandwidth-heavy generic path with two copies.
- The fused compute stage needs typed UAV stores for the colour, R32F and RG16F formats and typed
  UAV loads for the in-place original; `R10G10B10A2_TYPELESS` targets stay on the adapter paths. Its
  resolve interpolates the proxy and the model answer before composing, so outside the undeformed
  centre its result differs slightly from resolve-then-interpolate. Compare and debug views are
  unavailable on the fused stage.
- Runtime success in one game or on one GPU is not a certification: image-quality and frame-time A/B
  gates remain integration-specific.
