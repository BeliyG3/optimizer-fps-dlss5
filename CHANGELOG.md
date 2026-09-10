# Changelog

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
