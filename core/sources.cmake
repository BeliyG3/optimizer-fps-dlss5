set(OFPS_CORE_SOURCES
    context.cpp context.h log.cpp log.h frame/common.h frame/debug_readback.cpp
    frame/debug_readback.h frame/dispatch.cpp frame/dispatch.h frame/feature_state.cpp
    frame/feature_state.h frame/host_depth_state.cpp frame/host_depth_state.h frame/host_shape.cpp
    frame/host_shape.h frame/lifecycle.cpp frame/lifecycle.h frame/model_passes.cpp
    frame/model_passes.h frame/motion_smooth.cpp frame/motion_smooth.h frame/spread_model.cpp
    frame/spread_passes.cpp frame/spread_passes.h frame/timing.cpp frame/timing.h
    frame/warp_recorder.cpp frame/warp_recorder.h gpu/barriers.cpp gpu/barriers.h
    gpu/crash_guard_seh.cpp gpu/crash_guard_seh.h gpu/graveyard.cpp gpu/graveyard.h gpu/queues.cpp
    gpu/queues.h gpu/shaders.cpp gpu/shaders.h temporal/accumulation.cpp temporal/async_entry.cpp
    temporal/async_frame.h temporal/async_host_sync.cpp temporal/async_scheduler.cpp temporal/async_scheduler.h
    temporal/background.cpp temporal/controller.cpp temporal/controller.h temporal/history.cpp
    temporal/history.h temporal/machine.cpp temporal/machine.h temporal/mode_select.cpp
    temporal/phase.h temporal/residual.cpp temporal/resources.cpp temporal/resources.h
    temporal/resources_create.cpp shaders/temporal_layout.h shaders/temporal_passes.def)
list(APPEND OFPS_CORE_SOURCES api/ofps_core.h api/ofps_settings_schema.h settings/schema.cpp)
list(APPEND OFPS_CORE_SOURCES gpu/descriptor_pool.h gpu/descriptor_pool.cpp gpu/constant_ring.h gpu/constant_ring.cpp gpu/submission.h gpu/submission.cpp
    gpu/submission_recordings.cpp)
list(APPEND OFPS_CORE_SOURCES gpu/compute_pipeline.h gpu/compute_pipeline.cpp)
list(APPEND OFPS_CORE_SOURCES warp/path_policy.h warp/path_policy.cpp warp/compute.h warp/compute.cpp
    warp/resources.h warp/resources.cpp warp/format_support.h warp/format_support.cpp)

list(APPEND OFPS_CORE_SOURCES frame/model_protocol.h frame/model_protocol.cpp frame/codec_frame.h frame/codec_frame.cpp)
list(APPEND OFPS_CORE_SOURCES frame/dispatch_body.cpp)
list(APPEND OFPS_CORE_SOURCES frame/model_grid.h frame/model_grid.cpp)
list(APPEND OFPS_CORE_SOURCES frame/model_grid_path.h frame/model_grid_path.cpp)
list(APPEND OFPS_CORE_SOURCES frame/model_ui.h frame/model_ui.cpp)
list(APPEND OFPS_CORE_SOURCES frame/warp_codec.cpp)

list(APPEND OFPS_CORE_SOURCES temporal/async_codec.cpp)
list(APPEND OFPS_CORE_SOURCES temporal/flow.cpp)
list(APPEND OFPS_CORE_SOURCES temporal/diagnostic_keys.h temporal/diagnostics.h temporal/diagnostics.cpp temporal/profile.h temporal/profile.cpp)
list(APPEND OFPS_CORE_SOURCES temporal/stats.h temporal/stats.cpp)
list(APPEND OFPS_CORE_SOURCES temporal/pass_timing.h temporal/pass_timing.cpp)
list(APPEND OFPS_CORE_SOURCES flow/Backend.h flow/DriverBackend.cpp flow/OpticalFlow.h flow/OpticalFlowInternal.h flow/OpticalFlow.cpp flow/Submit.cpp flow/FrameState.h flow/FrameState.cpp flow/motion_source.h)

list(APPEND OFPS_CORE_SOURCES frame/frame_inputs.h frame/frame_inputs.cpp)

list(APPEND OFPS_CORE_SOURCES core_impl.h core_impl.cpp core_feature.cpp core_settings.cpp core_status.cpp settings/values.h settings/values.cpp settings/status.h settings/status.cpp)

list(APPEND OFPS_CORE_SOURCES frame/callback_guard.h)
