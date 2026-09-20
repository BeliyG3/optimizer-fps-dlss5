# Changelog

## 2026.9
_2026-09-20_ - the carry no longer depends on the depth convention, and the 32-bit tab gained the model passes.

**The release number changed shape.** Up to 26.29 it was the number of the work stage the build came
out of; "26" never meant anything and never moved. From this release it is the year and month, and a
second release inside one month adds a third component (2026.9.1). The SDK keeps its own semantic
version (PW_SDK_VERSION), which is unrelated.

**Fixed**

* **Depth of any convention.** The carry used a negative value as a marker for "this surface was not in
  the residual's frame", which is a real depth in a game that hands out linear view-space Z (007 First
  Light: -2.3 to -84). There the test rejected the model's contribution over the whole frame. The marker
  is a quiet NaN now, and "the expectation is off" travels in its own channel. On the bench, with the
  depth written as negative linear Z, acceptance is 93.8 % - the same as with reverse-Z.
* **Three defects in the history of the two previous passes.** The depth was read at a different place
  than the link it belonged to; neighbouring links were averaged into a displacement that pointed at
  neither surface; a NaN displacement quietly became zero, which reads as "did not move".
* **The history pass was handed a stand-in where it asked for the depth.** Storing a pass began to
  read this frame's depth, but the pass's list of inputs was not extended to match, and the machine
  blanks every input a pass does not declare - so the link between two stored passes recorded the
  residual's red channel as its source depth. The manifest now declares it, and a test compares the
  declared inputs against the compiled shaders, which is what caught this.
* **Guides read past the edge of their sub-rect.** The carry reaches outside the frame as a matter of
  course - the end of a motion chain, a ring tap around a rejected pixel - and those reads were not
  held inside the host's sub-rect. Past the edge of a texture a read returns zeros, and a zero depth
  is a real sample (the camera plane in a 0..1 buffer, infinity in a reversed one), so along a band at
  the frame's edge the tests either accepted a surface that was not there or threw away the pixel's
  own. Where a host keeps its guides in part of a larger texture, the same read reached into the
  neighbouring region instead. Every guide read and the bilinear chain fetch now clamp to the rect,
  as the packing shaders already did.

* **`Verify` threw instead of saying "unknown".** When a release's `VERSION.txt` could not be read -
  locked by another process, say - the verifier called a method on nothing and stopped. It now reports
  the version as unknown, as the installer already did, and the installer's test suite covers what the
  receipt records for a payload with, without and with a byte-order-marked version file.

**Added**

* **Model passes in the remote tab.** 32-bit games show the tab of the 64-bit host; it now carries
  `Model passes`, `Spread passes over frames`, the running count and the host's reasons, including
  why a temporal mode is not the one that was asked for. The shared block is protocol 3 and its
  section carries the version in its name, so a host and a tab of different ages no longer meet on
  the same memory - update both halves of a 32-bit install together.

**Changed**

* The temporal passes ship as compute shaders (`temporal_<Name>_cs.dxbc`, fifteen of them); the
  pixel-shader set is gone and the installer removes it.

## 26.28
_2026-09-19_ — a steadier picture between full model passes.

The temporal modes (Interpolate) used to show a small pulse once per cadence: every Nth frame the model
delivered a new opinion of the whole picture and the frames in between drifted away from it again. Four
changes from the experimental OptiScaler build are now in the add-on.

**Changed**

* **Expected depth.** The depth a surface had in the frame of the last full pass now travels with the
  motion chain, one link per frame, and the disocclusion test compares against that instead of against
  the surface's depth right now. Flying forward, a floor two metres ahead comes several percent closer
  every frame, so the old test threw the model's contribution away over more and more of the picture the
  longer since the pass — and took it all back at once on the next one.
* **A new pass fades in.** Over the shorter of the cadence and three frames, the frames show a mix that
  slides from the previous pass's contribution to the new one (the full frame included, which is now
  composed as "frame + the mix" rather than shown as the model's raw output).
* **Motion vectors for the model.** On a full pass the model is given the accumulated displacement so it
  can find its own history; where that chain does not end on the pixel's own surface, it now receives a
  vector that leaves the picture, so the model treats the pixel as a disocclusion of its own instead of
  blending its old picture of the occluder onto the wall he has left.
* **Rejected pixels are painted from cells.** What the reprojection accepted is averaged once per cell of
  a coarse grid together with the look of the pixels it came from; a rejected pixel takes a blend of the
  cells that resemble it instead of deciding from its own taps, which dithered in narrow bands.
* Better filling of rejected pixels, a veto on the neighbourhood average when the pixel itself is firmly
  rejected, stricter rules on silhouettes and two depth metrics instead of one (they also fix dotted dark
  outlines around lamps and high-contrast edges).
* The temporal passes now read the game's colour and depth through a proper barrier in the accumulation
  as well; before, only the reprojection transitioned them.

Measured on the bench (procedural 3D scene, 4K, `--fps-cap 60`, Peripheral 80/90, error against a 4K
reference render): with a full pass every 4 frames the centre error goes 7.07 → 7.00 and the periphery
8.22 → 8.44, while the frame-to-frame change of the error map — the flicker — drops from 1.93/2.28 to
1.80/2.18 (maxima, centre/periphery). Every 8 frames: 6.99 → 6.94 and 7.48 → 7.71, flicker 2.16/2.73 →
1.80/2.49. On a camera flying forward the error no longer climbs through the cadence and falls back at
each pass; the background mode is unchanged within its run-to-run spread.

**Notes**

* The background mode now keeps a pending expected-depth chain from the kick frame, promotes it with
  motion at adoption, and phases the new residual in from that frame's aligned predecessor.
  An interrupted fade starts from the outgoing mixture. Background model-motion validation runs on
  the host queue and copies to a private model input. The 2026-09-20 measurements with free GPU memory
  select expected depth and model-motion validation on, background phase-in off. The fade's small
  flicker benefit did not justify its higher mean error. Against a fresh all-off control, three-run
  median error (centre/periphery) improves from 4.297/4.146 to 3.191/3.280 in the default scene and
  from 3.321/3.136 to 2.959/2.823 in forward flight. Synchronous defaults are unchanged.
  Version 26.28 is not published yet: previously installed 26.28 copies predate the background fix
  and need this matched add-on/shader update despite carrying the same version number.
  The measurements above this note predate the extension. See `tools/bench/BACKGROUND_26_28.md`
  for the complete matrix, ranges, regression checks and rollout record.
* A layout change could clear the cached device pointers before the background queue check, causing
  one native frame to use synchronous interpolation. The check now restores the device pointers first.
  Retirement also signals pending model inputs on the observed host queue, avoiding a three-second
  wait when the queue registry uses the proxy device identity.
* Five new compiled shaders in `optimizer-fps-dlss5\` (15 in total). The add-on and the shaders are one
  unit — never copy one without the other; an add-on that finds no `temporal_expect_ps.dxbc` turns the
  temporal modes off and says so in the log.
* Read-only `[PeripheralWarp]` keys to switch the new passes off for a bisection:
  `DebugTemporalNoExpect`, `DebugTemporalPhaseIn` (0 = no fade-in), `DebugTemporalNoCells`,
  `DebugTemporalNoModelMotion`.

## 26.27
_2026-09-13_ — compatibility hotfix for renodx-dlss5 5.2.1.

**Fixed**

* renodx-dlss5 5.2.1 (2026-09-11) creates the Neural Rendering model at the render resolution first,
  commits the working resolution after a settle period and re-creates the model with worksets while the
  previous one is still alive. Models that belong to another consumer ("foreign" feature-18 instances)
  were passed to the original NGX entry points; the second creation failed with `0xBAD00002` and the
  consumer fell back to "install a Neural Rendering Consumer". Foreign models now go through the
  add-on's own forwarder, like the models the add-on manages itself. Verified on the bench with
  renodx-dlss5 4.55, 4.70 and 5.2.1.
* A per-frame `host resources` log line: the input signature was compared before it was cleared.

**Bench (`toolsench`, not shipped)**

* `--dlaa` (render size = output size, guides at native resolution), `--fps-jitter MS`, `--fullscreen`;
  the bench warns instead of exiting when the add-on is not loaded; a broken string literal that kept
  `pw_bench.cpp` from compiling is fixed.

No behaviour change for games where it already worked; settings, files and the installer are the same as 26.26.

## 26.26
_2026-09-10_ — first public release.

**Optimizer FPS for DLSS5** is a ReShade add-on that reduces the GPU cost of DLSS 5 Neural Rendering:
the screen periphery is compressed before the model runs and unpacked afterwards (the model processes
81 % of the pixels at 3840×2160 with the default 80/90 layout), and the model can run only every Nth
frame, with the frames in between reprojected along the game's motion vectors, either synchronously
or on a background GPU queue.

**In the zip:** `Install-OptimizerFPS.cmd` (drag the game's `.exe` onto it), `Verify-OptimizerFPS.ps1`,
`README.txt`, and the payload: `optimizer-fps-dlss5.addon64`, the NGX forwarder `nvngx.dll_optimizerfps.dll`,
10 compiled shaders in `optimizer-fps-dlss5\`, and `optimizer-fps-dlss5-remote.addon32` for 32-bit games.

**Needs:** Windows 10/11 64-bit, an NVIDIA RTX GPU (developed on an RTX 4080 SUPER), ReShade 6.8+ with
add-on support, `nvngx_dlssnr.dll` 310.8, and a working Neural Rendering consumer (renodx-dlss5,
DLSS5-Feeder's `host64`, or the OptiScaler DLSSNR fork). Do not use it in games with anti-cheat.

Settings live in the `[PeripheralWarp]` section of `ReShade.ini` (the section keeps the project's
historical internal name).
