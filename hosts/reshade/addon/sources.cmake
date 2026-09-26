# Sources of the ReShade add-on shell (entry point, settings, tab, crash guard, remote host).
# Paths are relative to hosts/reshade. Stage 27.D2 split producer.cpp into the modules below;
# docs/dev/ADDON_MODULES.md says what each one is responsible for.
set(OFPS_ADDON_SOURCES
    addon/addon_context.h
    addon/addon_main.cpp
    addon/config_store.cpp
    addon/config_store.h
    addon/crash_guard.cpp
    addon/crash_guard.h
    addon/exports.cpp
    addon/ini_schema.h
    addon/ini_migration.h
    addon/ini_store.cpp addon/ini_store.h
    addon/shell_settings.cpp addon/shell_settings.h
    addon/status_log.cpp addon/status_log.h
    addon/layout_bridge_v1.h
    addon/optiscaler_link.cpp addon/optiscaler_link.h
    addon/overlay.cpp addon/overlay_diagnostics.cpp
    addon/overlay_schema.cpp addon/overlay_schema.h
    addon/overlay.h
    addon/overlay_temporal.cpp
    addon/overlay_temporal.h
    addon/overlay_zone.cpp
    addon/overlay_zone.h
    addon/queue_events.cpp
    addon/queue_events.h
    addon/remote_host.cpp
    addon/remote_host.h
    addon/remote_host_snapshot.cpp
    addon/remote_host_snapshot.h
    ../common/overlay_widgets.cpp
    ../common/overlay_widgets.h
    ../remote32/ipc.h
    feeder_ofa_cfg.h)
