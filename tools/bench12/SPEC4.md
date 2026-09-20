# pw_bench12, milestone 4: interactive mode (the user tunes the scene by hand)

Read `SPEC.md` .. `SPEC3.md`, `README.md` and the sources first. Same rules (module layout, files under ~300 lines, README and
tests updated, /W4 /WX, nothing outside `tools/bench12`). Dear ImGui 1.92.5-docking is unpacked in `external/imgui/` (MIT):
compile `imgui.cpp imgui_draw.cpp imgui_tables.cpp imgui_widgets.cpp backends/imgui_impl_win32.cpp backends/imgui_impl_dx12.cpp`
with warnings relaxed for those files only. `external/` is not tracked: add `/external/` to `.gitignore` and say in the README
where ImGui comes from.

## Launch
- `--interactive` : no frame limit, resizable window (recreate swap chain and render targets, NGX feature re-created at the
  new size), vsync on by default, the menu visible (F1 toggles it). Everything else stays scriptable exactly as today:
  without `--interactive` the behaviour, outputs and dumps must not change.
- `run_lab.bat` next to the exe: starts `pw_bench12.exe --interactive --gltf ..\bench\assets\lab_scene.glb --upscaler rr`
  with the current good defaults (`--sun-dir 0.45,-0.77,0.45 --sun-strength 1500 --exposure 0.22 --haze 0.008 --fov 62`)
  and `pause` on a non-zero exit code so the error stays readable.
- `settings.json` next to the exe (hand-written tiny JSON reader/writer, no library): everything the menu changes, the camera
  bookmarks and the lamp groups. Loaded at start in interactive mode (command-line options given explicitly win), written by
  the "Save" button and on exit. A `--settings file` option lets the scripted mode load the same file, so what the user tuned
  by hand is what the automated runs use.

## Camera: free flight
Right mouse button held = mouse look (hide and recentre the cursor), WASD move, Q/E down/up, Shift x4, Ctrl x0.25, mouse wheel
changes the base speed. `Home` returns to the third-person view behind the character (the scripted static camera). `F5`..`F8`
recall bookmarks 1..4, Ctrl+`F5`..`F8` store them. Camera motion produces correct previous-frame matrices (motion vectors!) and
resets accumulation. ImGui's WantCaptureMouse/Keyboard are respected.

## Lamps
- A lamp group = all emissive triangles that share a glTF material name (`pw_gltf.h` Primitive has `materialName`; carry a group
  index per emissive triangle). Expected groups in the lab scene: lab_tube, lab_ring, lab_spot, lab_screen, lab_sign,
  lab_booth_sign (names may carry a `.001` suffix: strip a trailing `.NNN` when grouping).
- Per group: `intensity` multiplier (0..20, logarithmic slider, default 1) and `tint` RGB (colour picker, default white), applied
  on top of the material's emission. Applying a change must be cheap: keep emission = base x group factor in the shader via a
  small group table (StructuredBuffer or constants), and rebuild the power CDF on the CPU when a factor changes (debounce: at
  most once per 100 ms while a slider is dragged), re-uploading the lights buffer.
- Picking: left click on the image (when ImGui does not want the mouse) casts the camera ray through that pixel on the CPU
  side? No: read back. Add to the path-trace shader a tiny "pick" output: a 1x1 request (pixel coordinate in the constants,
  -1 = none) writes the primary hit's triangle index and lamp group (or 0xFFFFFFFF) to a small UAV buffer; read it back next
  frame. A click on an emissive surface selects its group: the menu scrolls to it and highlights it; the selected group is
  outlined on screen by boosting/flashing it for 0.3 s (optional).
- Menu "Lamps": the list of groups with triangle count and total power, the two controls, "Solo" (others off) and "Reset".

## Menu "Render"
- Upscaler: None / DLSS SR / DLSS RR (switching re-creates the NGX feature; failure shows the error text in the menu and falls
  back to None, the app keeps running). DLSS mode: Performance 0.5 / Balanced 0.58 / Quality 0.667 / DLAA 1.0 (render scale).
- Paths per pixel (spp 1..16), bounces 0..8, light candidates 1..32.
- Accumulation: Off / N frames (slider 2..4096, "infinite" checkbox). Accumulation averages the path-traced colour while the
  camera and settings do not change, restarts on any change; when on, what is shown (and what is fed to the upscaler - make it
  a checkbox "feed the upscaler the accumulated image", default off) is the running average. Show "accumulated k / N".
- Sun: strength (log slider), direction as azimuth/elevation, angular size. Haze density and anisotropy. Exposure (log), auto
  exposure, bloom, tone map. Firefly clamp.
- View: Final, Noisy input, Accumulated, Depth, Motion vectors, Normals, Roughness, Diffuse albedo, Specular albedo.
- Overlay (always visible, top-left, toggle F2): render size -> output size, upscaler, path trace ms, upscaler ms, frame ms, fps,
  accumulated frames.
- Buttons: Save settings, Reload settings, Dump frame (writes dump_<n>.bmp like --dump).

## Known problem to fix in this milestone
The process crashes at exit after RR has run (access violation after the last frame, even after the milestone-3 change). In
interactive mode this is hit on every window close. Find it: suspect order of `NVSDK_NGX_D3D12_ReleaseFeature`/`Shutdown1`
versus the last GPU work and device destruction, a double release of the parameter block, or the log callback being called
after its owner is gone (the init may now run WITHOUT the feature-info block: see the retry in ngx.cpp and keep it).
If you cannot find it by reading, make shutdown robust: wait for the GPU, release the feature, destroy parameters, call
Shutdown1(device), then release D3D12 objects; and as a last resort terminate the process with `TerminateProcess` after
everything has been flushed and saved, with a comment explaining why.

## Verification
Build with the full path of build.cmd; CPU tests pass (add tests for the settings JSON round trip, lamp grouping by material
name with `.NNN` suffixes, CDF rebuild with group factors, azimuth/elevation <-> direction). List what needs a GPU to verify.
