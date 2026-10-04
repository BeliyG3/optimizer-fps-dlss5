# Temporal passes before the carried-frame grid

`temporal_Reproject_cs.dxbc`, `temporal_Cells_cs.dxbc` and `temporal_Compose_cs.dxbc` are the baseline from
before the grid: built from `core/shaders` and `sdk/shaders` as they were just before it was added (development
commit e1e1dd5, which is not in the public history) with the flags of
`cmake/CompileShaders.cmake` (`fxc /Ges /WX /O3 /T cs_5_0`, fxc 10.1 from Windows SDK 10.0.26100).
`ofps_temporal_grid_shaders` runs them on WARP beside the current build: with the grid off, the current passes
must write the same bytes. Replace them only when one of these passes changes on purpose.
