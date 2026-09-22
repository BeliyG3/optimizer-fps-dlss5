# Sources of the NGX hook (feature-18 interception, warp recording, temporal machine, background pass).
# Paths are relative to adapters/reshade. Stage 27.D moves code from ngx_hook.cpp into this folder.
set(PW_NGX_SOURCES
    ngx/async_scheduler.cpp
    ngx/async_scheduler.h
    ngx/debug_readback.cpp
    ngx/debug_readback.h
    ngx/feature_state.cpp
    ngx/feature_state.h
    ngx/hook_common.h
    ngx/hook_context.cpp
    ngx/hook_context.h
    ngx/hook_dispatch.cpp
    ngx/hook_dispatch.h
    ngx/host_depth_state.cpp
    ngx/host_depth_state.h
    ngx/ngx_common.cpp
    ngx/ngx_common.h
    ngx/ngx_hook_api.cpp
    ngx/ngx_params.cpp
    ngx/ngx_params.h
    ngx/ngx_temporal.cpp
    ngx/ngx_temporal.h
    ngx/temporal_controller.cpp
    ngx/temporal_controller.h
    ngx/temporal_background.cpp
    ngx/temporal_accumulation.cpp
    ngx/temporal_residual.cpp
    ngx/temporal_phase.h
    ngx/temporal_history.h
    ngx/temporal_history.cpp
    ngx/temporal_resources.cpp
    ngx/temporal_resources_create.cpp
    ngx/temporal_resources.h
    ngx/timing.cpp
    ngx/timing.h
    ngx/warp_compute.cpp
    ngx/warp_compute.h
    ngx/warp_recorder.cpp
    ngx/warp_recorder.h
    ngx_hook.h)

list(APPEND PW_NGX_SOURCES ${CMAKE_CURRENT_LIST_DIR}/model_passes.cpp)
list(APPEND PW_NGX_SOURCES ${CMAKE_CURRENT_LIST_DIR}/spread_passes.cpp ${CMAKE_CURRENT_LIST_DIR}/spread_model.cpp)
