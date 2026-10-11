# Temporal passes before the sky field

`temporal_Reproject_cs.dxbc`, `temporal_Compose_cs.dxbc`, `temporal_ComposeGrid_cs.dxbc` and
`temporal_Apply_cs.dxbc` are the passes as they were on master `e6f9e0e`, just before the sky field
(`TemporalSkyField`, 2026.10.2), built with the flags of `cmake/CompileShaders.cmake` (`fxc /Ges /WX /O3 /T cs_5_0`,
fxc 10.1 from Windows SDK 10.0.26100). They are the only passes whose instructions the sky field changed; every
other temporal pass differs from master in its constant-buffer reflection alone.

`ofps_temporal_sky_shaders` runs them on WARP beside the current build (the sky field unbound, and bound to frames
without a far-plane pixel), and `ofps_temporal_sky` runs whole machine sequences with them in place of the current
four, `TemporalSkyField` off from the first frame: the current passes must write the same bytes. Replace them only
when one of these passes changes on purpose.
