# Menu mode

Some games stop calling Neural Rendering in their menus — the pause menu, map and workbench of Star Wars Jedi:
Fallen Order, the menus of Baldur's Gate 3 — and the menu background loses the NR look. With **Menu mode** on
(`MenuMode=1`, a checkbox under Mode) the add-on runs the NR model on the frame the game presents while the game does
not.

- It starts on its own at least 150 ms and 5 presents after the game's last NR frame, and ends at the game's next NR
  frame. That frame restarts NR's history, because the menu frames were fed to NR as they are.
- The model sees the whole presented frame as it is, the menu's text and panels included; no colour correction.
- It follows your Mode. With Off the model runs at the frame's size. With Uniform or Peripheral the menu frame is
  shrunk, run through the model and rebuilt exactly like a game frame, with the same layout and filter.
- It follows your temporal setting too (see below).
- Native D3D12 swap chains, and D3D11 swap chains whose NR runs on a D3D12 device through a bridge
  (OptiScaler's D3D11 path in Star Wars Jedi: Fallen Order, and dlss5-dx11-bridge on the bench). The GPU orders the
  work; the present thread never waits for it.
- One swap chain at a time: the first one menu mode sets itself up for, until the game closes it. A second window of
  the game (a launcher, a video) is left alone meanwhile.
- Each new "unavailable" reason is also written to `ReShade.log` once.

## Temporal modes in menus

- **Every frame** (`TemporalMode` 0): the model runs on every menu frame.
- **Interpolate (sync)** (`TemporalMode` 1 or 2): the model runs every N-th menu frame (your N, `TemporalEvery`) and
  the frames in between carry its edit. Menus have no game motion, so the edit is moved by NVIDIA's optical flow
  instead. This works with Off, Uniform and Peripheral, on D3D12 and on D3D11 through a bridge. On the bench it costs
  about half the GPU time of running the model every frame.
- **Interpolate (background)** (`TemporalMode` 3) is never used in menus. With Mode Off the model runs on every menu
  frame; with Uniform or Peripheral menu mode says "the background temporal mode is not used in menus: use Every frame
  or Interpolate (sync)".
- **No optical flow** (a GPU without the optical flow engine, a driver without the D3D12 optical flow API, or the
  optical flow failing in a menu): nothing is carried. With Mode Off the model runs on every menu frame and the tab adds
  "temporal cadence off: optical flow is not available". With Uniform or Peripheral menu mode says "unavailable:
  optical flow is not available". The state is kept until the game restarts and is written to `ReShade.log` once.

## Unavailable

Menu mode is unavailable, with the reason under the checkbox, in these common cases (rarer ones, such as a D3D11
bridge that cannot share the back buffer's format, are shown the same way):

| Case | Reason shown |
|---|---|
| the background temporal mode (3) with Uniform or Peripheral | not used in menus: use Every frame or Interpolate (sync) |
| Interpolate (sync) with Uniform or Peripheral and no optical flow | optical flow is not available |
| an HDR10 swap chain, or scRGB without FP16 | the colour space; retried automatically when the game switches back |
| the game's NR input is a part of a larger texture | the game's NR input is not the whole frame |
| the game hands NR a UI or back-buffer texture | not supported in menus |
| the game's depth is a depth-stencil texture | not readable in menus |
| a setting changed inside the menu | it takes effect from the game's next frame; retried automatically |
| a 32-bit game through DLSS5-Feeder | the checkbox itself says "not for 32-bit games" |
| OptiScaler applies the settings itself (direct host) | OptiScaler runs the model itself here; its own menu shows the checkbox as unavailable |

## Cost and limits

Cost in a menu: one model pass per presented frame (about 6 ms at 1080p and 17 ms at 4K on an RTX 4080 SUPER, bench);
with Interpolate (sync) about half of that on average. While the checkbox is on: a depth copy per game frame and about
200 MB of textures at 4K; after it is turned off the textures stay allocated until the game closes its NR feature or
its window. If three menu passes in a row take 100 ms or more (a GPU short of memory), that menu stays without NR and
the next menu tries again. Turn menu mode on during gameplay: in a menu that is already open when you tick the box,
it starts with the next menu (it needs one game frame after the box is ticked).

On a slow GPU (a menu pass of more than 100 ms) the game's first NR frames after a menu wait until the last menu pass
has finished: those frames get no NR (the game sees an NR failure for them). Menu mode stays on. Menu frames that go
through the core count as warped frames in the tab's counters.

Two costs come with the temporal cadence: the first menu of a game session can hitch once (about 60 ms) while the
optical flow session is created, and the first frames after leaving a menu look softer than without a temporal mode.

## Where it was checked

In a game: Star Wars Jedi: Fallen Order (OptiScaler's D3D11 path, Luma's scRGB HDR). Not checked yet: the menus of
Baldur's Gate 3, and DLSS5-Reshade-AIO or DLSS5-Feeder hosts.

Limits are in [LIMITATIONS.md](LIMITATIONS.md#menu-mode); the bench results and the remaining in-game checks are in
[dev/menu-mode-acceptance.md](dev/menu-mode-acceptance.md). The temporal modes themselves are in
[TEMPORAL_MODES.md](TEMPORAL_MODES.md).
