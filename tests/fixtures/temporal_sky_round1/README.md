# Frame passes of the sky field's first version

`temporal_Reproject_cs.dxbc`, `temporal_Compose_cs.dxbc` and `temporal_ComposeGrid_cs.dxbc` are the passes as they
were on `fix/sky-field` at `4e61c6b` (sky field fix round 1): a far-plane pixel of a carried frame took the sky field
alone whenever the field had 2 % support. Built with the flags of `cmake/CompileShaders.cmake`
(`fxc /Ges /WX /O3 /T cs_5_0`, fxc 10.1 from Windows SDK 10.0.26100). Apply, ReprojectGrid and the field's own passes
compile to the same bytes since.

Fix round 2 weighs the field by the share of sky the full pass saw against the carried frame's own
(`temporal_sky.hlsli`, `PwTSkyWeight`). `ofps_temporal_sky_shaders` runs these three beside the current build: where
the weight is 1 (the pass saw 40 % sky, the frame shows 45 %) the current passes must write their bytes. Replace them
only when the weight-1 route changes on purpose.
