# Porting checklist

- [ ] Identify the executable architecture and graphics API.
- [ ] Obtain the color, depth, motion, confidence, subrects, and semantics from the existing DLSS integration; do not rediscover game buffers.
- [ ] Describe the actual typed SRV format for every resource; never infer a view for a typeless allocation.
- [ ] Confirm color encoding, depth convention, motion direction/units, jitter treatment, and independent guide resolutions.
- [ ] Build `FrameInputV2` and let Pack normalize guides directly into the canonical formats.
- [ ] Use ABI v3 when Global scale or Work below the legacy guard is required; keep ABI v1/v2 callers unchanged.
- [ ] Build and validate raw and effective layout dimensions from the native dimensions.
- [ ] Enforce `Center < Work`, `Work >= 25%`, and `Global * Work >= 25%` on each axis.
- [ ] Pack all guides from the same frame on the caller's command list.
- [ ] Create the temporal renderer at the exact work dimensions.
- [ ] Unpack color and guides before native-coordinate consumers.
- [ ] Draw optional Center and raw Work outlines only after Unpack and without another fullscreen pass.
- [ ] Normalize reconstructed native-pixel motion correctly for frame generation.
- [ ] Reset temporal history once after resize, layout change, skipped frame, or re-enable.
- [ ] Ensure bypass always displays a reconstructed/native image, never packed color.
- [ ] Verify frame-slot retirement without CPU/GPU waits in the SDK.
- [ ] Test unsupported formats and missing depth/MV as signature-scoped native-resolution fallback.
- [ ] Test install, hash verification, and byte-identical rollback in a disposable copy.
- [ ] A/B the same camera and scene with the warp off and on, with and without frame generation:
      median and p95 GPU frame time, the centre at the 80 % boundary, and the periphery in motion.
