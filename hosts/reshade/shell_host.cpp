#include "hosts/reshade/shell_host.h"
#include "hosts/reshade/direct_host.h"
#include "core/api/ofps_core_loader.h"
#include "ofps_version.h"
#include "hosts/reshade/addon/addon_context.h"
#include "hosts/reshade/addon/config_store.h"
#include "hosts/reshade/addon/ini_store.h"
#include "hosts/reshade/addon/crash_guard.h"
#include "hosts/reshade/ngx_hook_api.h"
#include <atomic>
#include <cstdio>
#include <cstring>


namespace ofps::reshade {
namespace {
ofps::CoreLoader loader;
std::atomic<bool> deviceRemoved{false};
} // namespace
ShellHost &Host() {
    static ShellHost host;
    return host;
}
IOfpsCore *Core() { return loader.Get(); }
const char *CoreLoadError() { return loader.Error().c_str(); }
bool ShellDeviceRemoved() { return deviceRemoved; }
int CoreAttach() {
    wchar_t path[MAX_PATH * 4]{};
    const DWORD length = GetModuleFileNameW(State().module, path, static_cast<DWORD>(std::size(path)));
    const int result = loader.Attach(length && length < std::size(path) ? path : L"", "ReShade", &Host());
    if (result < 0) {
        const std::string message = "Optimizer FPS: core load failed: " + loader.Error();
        Host().Log(OFPS_LOG_WARN, message.c_str());
    } else {
        Host().Log(OFPS_LOG_INFO, "Optimizer FPS: core loaded; ABI 1; release " OFPS_ADDON_VERSION_STRING);
    }
    return result;
}
bool CoreDetach(bool serialize) {
    return loader.Detach(serialize);
}
void ShellHost::Log(OfpsLogLevel level, const char *text) {
    // The ABI carries model results; the shell alone owns the native NGX diagnostic code.
    const bool nativeResult = std::strstr(text, "layout changed, model re-created at") ||
                              std::strstr(text, "model re-created at native size after a failure") ||
                              std::strstr(text, "feature 18 create failed (");
    char message[4096];
    if (nativeResult) {
        const char *begin = std::strchr(text, '(');
        const char *end = begin ? std::strchr(begin, ')') : nullptr;
        if (end) {
            std::snprintf(message, sizeof(message), "%.*s%d%s", static_cast<int>(begin + 1 - text),
                          text, ShellState().lastNgxResult, end);
            text = message;
        }
    }
    LogForNgxHook(level != OFPS_LOG_INFO, text);
}
void ShellHost::OnEvent(OfpsEvent kind, const OfpsEventData *data) {
    if (!data)
        return;
    switch (kind) {
    case OFPS_EVENT_FIRST_WARPED_FRAME:
        CrashMarkerOnFirstWarped();
        break;
    case OFPS_EVENT_SETTINGS_CHANGED:
        if (!DirectHostActive() && !IniSaveSuppressed() && data->payload &&
            CurrentIniSelection() == IniSelection::New) {
            const auto &values = *static_cast<const OfpsSettingsValues *>(data->payload);
            if (CurrentSettingSaveFilter() >= 0)
                SaveOneSettingToReShadeIni(
                    static_cast<std::uint32_t>(CurrentSettingSaveFilter()), values);
            else
                SaveChangedExplicitValuesToReShadeIni(values);
        }
        break;
    case OFPS_EVENT_FEATURE_RELEASED:
        ForgetModelHost(data->handle);
        break;
    case OFPS_EVENT_DEVICE_REMOVED:
        deviceRemoved = true;
        break;
    case OFPS_EVENT_HOST_SHAPE_REJECTED:
        SetHookReason(data->text);
        break;
    default:
        break;
    }
}
} // namespace ofps::reshade
