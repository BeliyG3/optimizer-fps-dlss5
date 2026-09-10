# Sources of the ReShade add-on shell (entry point, settings, tab, bridge, crash guard, remote host).
# Paths are relative to adapters/reshade. Stage 27.D2 split producer.cpp into the modules below;
# docs/dev/ADDON_MODULES.md says what each one is responsible for.
set(PW_ADDON_SOURCES
    addon/addon_context.h
    addon/addon_main.cpp
    addon/config_store.cpp
    addon/config_store.h
    addon/crash_guard.cpp
    addon/crash_guard.h
    addon/exports.cpp
    addon/ini_schema.h
    addon/layout_bridge.cpp
    addon/layout_bridge.h
    addon/overlay.cpp
    addon/overlay.h
    addon/queue_events.cpp
    addon/queue_events.h
    addon/remote_host.cpp
    addon/remote_host.h
    diagnostics.h
    pw_remote_ipc.h
    pw_ofa_cfg.h)
