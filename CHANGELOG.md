# Changelog

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
