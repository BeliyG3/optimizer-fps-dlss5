# Temporal modes

The "Temporal" group has four settings; everything else in the machine is fixed at values verified
on the bench and in game.

* **Temporal mode** (`TemporalMode`) — `Every frame`; `Interpolate: full NR every N-th frame (sync)`;
  `Interpolate: model in the background (async)`.
* **N** (`TemporalEvery`) — 2…8 sync, 1…8 background.
* **GPU frames queued ahead** (`TemporalMaxQueue`, background only, default 2).
* **Carry at the model's resolution** (`TemporalGrid`, default on) — see
  [Carried frames at the model's resolution](#carried-frames-at-the-models-resolution).

Fixed: depth tolerance 0.05, color tolerance 0.08 (chromaticity first, luma at twice the
tolerance), no motion limit, hole fill on, Catmull-Rom resampling on, warp base on, residual age
limit 8 frames in background mode, guided smoothing radius 24 px, motion-vector search radius 16 px.

When Mode is `Off` the interposer still drives the cadence, on the native model.

## Interpolate (sync)

A full pass runs every N-th frame and records the residual `R = output − color`. Frames in between
show the current color plus `R` re-projected along the host's motion vectors, accumulated over the
skipped frames. The residual is dropped where the current depth no longer matches the residual
frame's depth at the re-projected position, where the color no longer matches (which catches wrong
vectors, disocclusions the depth test misses, HUD and transparency), and at every rejected link of
the displacement chain — the chain is validated step by step, not only at its endpoint, so a pixel an
occluder crossed does not inherit the occluder's history.

Rejected pixels are not left without the model's contribution: they take a box-filtered residual
(native/16 per axis, refreshed on every full pass), searched around the reprojected position in two
rings of eight points at 24 and 48 px for a depth match, scaled by the luma ratio to the current
frame and applied along the pixel's own chroma. Without that fill they show the raw color, which in
a tone-mapping host is 5–8 % darker than its surroundings — a dark rim along every moving silhouette.

Three refinements matter for how it looks: the acceptance weight is averaged over nine taps 3 px
apart, so the boundary between reprojected residual and fill is a ramp that does not jitter frame to
frame; the depth test falls off smoothly from the tolerance to twice it rather than cutting, because
thin geometry gives noisy depth; and the compose pass smooths the applied addition over 16 taps on a
noise-rotated disc where acceptance is below 0.5, weighting each tap by the current frame's own
color, depth and acceptance.

On every full pass the new residual is blended with the previous one moved to this pass's frame,
Catmull-Rom resampled, clamped to the 3×3 neighbourhood of the new residual and only where the depth
still matches. The weight is adaptive: 0.6 where the pixel moved less than 2 px and its color is
unchanged, fading out by 6 px of motion or half the color tolerance. This is what stops the teeth,
nostrils and lips of a talking face snapping between pass-to-pass versions of the model's output.

Frame times alternate long/short. Fine detail on moving objects refreshes at the full-pass rate.

## Interpolate (background)

The model runs on its own D3D12 queue on private copies of the host's color, depth and motion — and,
in the warped path, Pack → model → Unpack all on that queue. One pass is in flight at a time; N sets
how often a new one starts (1 = continuous). Every displayed frame is the current color plus the
last finished pass's residual moved along the accumulated vectors, with the hole fill; the plain
color until the first pass completes.

Expected depth travels with both chains and is promoted with the finished pass. Expected depth and
model-motion validation are enabled by default. Background phase-in is off: repeated measurements
with free GPU memory favoured the mean error of the two-feature combination. An explicit
`DebugTemporalPhaseIn=-1` enables the automatic adoption fade over `min(N-1, 3)` carried frames;
a positive value selects its length. Synchronous phase-in remains automatic by default.
`DebugTemporalNoModelMotion=1` disables validation in both modes; an absent key enables it.
See [the background bench report](../tools/bench/BACKGROUND_26_28.md).

Two accumulation chains run: one to the residual on screen, one to the frame of the pass in flight.
The second becomes the first when a pass is adopted, which is what gives each new pass the
displacement to the *previous pass's* frame rather than a one-frame vector — without it the model's
history is misaligned by age−1 frames on every pass.

Synchronization is queue-side only: the kick's host list is tagged with private data, ReShade's
`execute_command_list` event records the queue, the signal is issued at the next evaluate so it is
queued behind the list, and the background queue waits on it. The host queue waits for the pass only
when the residual reaches the age limit. On a layout change, a re-created model or a released
feature, the pass in flight is waited for on the CPU and whatever it still uses goes to the
fence-gated graveyard.

The queue asks for `GLOBAL_REALTIME` priority (`SeIncreaseBasePriorityPrivilege` is requested first,
`HIGH` is the fallback) and logs the outcome as `background queue priority: …`.

**It needs a registered host queue.** Where the consumer creates its D3D12 device before ReShade is
in place — always the case when OptiScaler loads ReShade — no queue is ever registered, the synchronous
interpolation runs instead, and both the tab and the log say `background mode unavailable here (no
registered host queue)`.

Where the residual's age comes from: the kick's copies sit in the host's list, which the GPU reaches
only after the frames already queued ahead of it (5–6 in BG3), then the pass itself, then up to one
frame until the next evaluate notices. Since the next kick starts at the adoption, the residual on
screen ages from `a` to `2a−1`. `GPU frames queued ahead` attacks the first part: a fence per
evaluate on the host's queue, and the CPU waits before recording a frame until at most that many are
unfinished — the GPU stays busy, the queue stops piling up, input latency drops. An in-game frame-rate
cap does the same job. On activation the CPU also waits once for everything the host has queued.

Frame times are more even, which is the point: the synchronous mode's long/short alternation reads as
judder even when the average is higher.

## Carried frames at the model's resolution

With Uniform or Peripheral compressing the frame for the model, the model's edit never has more detail than the
model's own, smaller frame. **Carry at the model's resolution** (`TemporalGrid=1`, on by default) uses that on carried
frames, in both Interpolate modes: the reprojection, which decides per pixel where the last pass's edit comes from and
whether it still fits, runs on a coarser grid matched to the model's resolution (in Peripheral, to its 1:1 centre)
instead of on every native pixel, and its result is read back smoothly into the native frame. The frame itself and the
compose step that smooths the edit stay at native resolution, and frames where the model runs are not affected.

It applies only where the model really is smaller: when the model's frame is at most 80 % of the screen on each
axis (Peripheral: its 1:1 centre after Global scale; Uniform: Work × Global scale, so Uniform with Work at 80 % or
less uses it even at Global scale 100). Mode Off and a larger model, such as the default Peripheral 80/90 at
Global scale 100, stay at native resolution. The price is a slightly softer edit on carried frames at moving
edges; untick the checkbox (`TemporalGrid=0`) and carried frames are exactly as before. The log says
`temporal grid WxH for native WxH (model WxH)` whenever the grid size changes (`0x0` = native, which is also what
it says for an older install that lacks the three grid shader files: it keeps the native reprojection).

## The warped base

With the warp on, the model's output is the packed frame unpacked, so a residual measured against
the host's raw color would also contain `unpack(pack(x)) − x` — the compression blur of the
periphery, a function of screen position rather than of the scene. Re-projected along the motion
vectors it landed on the sharp color of another position, which showed as periphery shimmer at the
full-pass rate and trails along moving edges, only with the warp on. Every temporal frame is now
based on the frame's own color through Pack → Unpack without the model (an extra Pack + Unpack on
interpolated frames, ~0.5 ms), so the residual holds only the model's contribution.

## Measured

RTX 4080 SUPER, RenoDX NR on a 3456×1944 packed frame: a full pass costs 13–15 ms; Interpolate lifts
53 fps (18.8 ms) to 86 (11.6 ms) at N=2, 109 (9.2 ms) at N=3 and 127 fps (7.9 ms) at N=4. Warped, at
a 60 fps budget, an interpolated frame against every-frame scores periphery edge correlation 0.976
(sync) and 0.974 (background), the center unchanged at 0.98.

The model's own tone statistics lag when frames are skipped: a full pass after N−1 skipped frames
comes out darker than every-frame while the scene changes (about 0.3–0.6 % at N=2, up to 2.7 % at
N=4 right after a scene change), regardless of which vectors it is given. With the consumer's
Intensity / LocalTone / LocalStructure at 0 the difference disappears. N=2 and Interpolate are the
recommended starting point.

## In menus

With menu mode on, Interpolate (sync) also runs in menus: the model every N-th menu frame, and the frames in between
carry its edit, moved by NVIDIA's optical flow because menus have no game motion vectors. The background mode is not
used in menus. Without optical flow the frames are not carried and the model runs on every menu frame; see
[MENU_MODE.md](MENU_MODE.md).
