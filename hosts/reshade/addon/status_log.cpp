#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <reshade.hpp>

#include "status_log.h"

#include "../ngx_hook_api.h"
#include "../shell_host.h"

#include <cstdint>
#include <cstdio>
#include <mutex>

namespace ofps::reshade {

void LogStatusPeriodically()
{
    static std::mutex s_mutex; // several effect runtimes can present on different threads
    std::lock_guard lock(s_mutex);
    static ULONGLONG s_last = 0;
    static bool s_started = false;
    static std::uint64_t s_handled = 0, s_fallback = 0;
    const ULONGLONG now = GetTickCount64();
    if (s_started && now - s_last < 30000) return;
    IOfpsCore *core = Core();
    if (core == nullptr) return;
    OfpsStatus s{};
    s.size = sizeof(s);
    core->Status(&s);
    if (!s.featureCreated) return;
    s_last = now;
    if (!s_started) { // the first line covers a full 30 s, not everything since start-up
        s_started = true;
        s_handled = s.evaluations;
        s_fallback = s.fallbackFrames;
        return;
    }
    const char *state = SafeMode() ? "SAFE MODE (crash guard)" : s.active ? "ACTIVE" : "NOT ACTIVE";
    char reason[256]; // the core's buffer is rewritten by the next evaluate; take a copy now
    std::snprintf(reason, sizeof(reason), "%s", s.reason != nullptr ? s.reason : "");
    char line[768];
    std::snprintf(line, sizeof(line),
                  "Optimizer FPS status: %s; last 30 s: %llu frames through the add-on, %llu shown as plain colour; "
                  "model %ux%u of %ux%u, temporal mode %u%s%s",
                  state, static_cast<unsigned long long>(s.evaluations - s_handled),
                  static_cast<unsigned long long>(s.fallbackFrames - s_fallback), s.workW, s.workH, s.nativeW,
                  s.nativeH, s.temporalMode, reason[0] != 0 ? "; reason: " : "", reason);
    s_handled = s.evaluations;
    s_fallback = s.fallbackFrames;
    ::reshade::log::message(::reshade::log::level::info, line);
}

} // namespace ofps::reshade
