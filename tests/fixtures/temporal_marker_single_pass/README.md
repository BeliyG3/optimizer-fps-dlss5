# The marker passes before the two-pass V3

`temporal_ModelMotion_cs.dxbc` and `temporal_MarkerReason_cs.dxbc` are the passes as they were at `672eb87`, where
rules 3/4 (`DebugTemporalMarkerRule`) ran the base test of every neighbour inline, built with the flags of
`cmake/CompileShaders.cmake` (`fxc /Ges /WX /O3 /T cs_5_0`, fxc 10.1 from Windows SDK 10.0.26100). They read
neither t1 nor `PwTParams.x`.

`ofps_temporal_marker_reason` runs them on WARP beside the current two-pass route (MarkerFail, then the pass with
the fail mask at t1) and the current single-pass fallback: every pixel of ModelMotion and of the reason plane must
be the same bytes, for every rule and threshold. Replace them only when the marker's decisions change on purpose.
