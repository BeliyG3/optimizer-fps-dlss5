#include "hosts/reshade/menu_settings.h"
#include <atomic>
#include <mutex>

namespace ofps::reshade {
namespace {
std::atomic<bool> g_menuMode{false};
std::atomic<int> g_temporalMode{0};
std::atomic<std::uint64_t> g_epoch{0};
std::mutex g_lastMutex; // leaf: taken under the core's lock only
OfpsSettingsValues g_last{};
bool g_haveLast = false;

// Any value other than MenuMode differs (bit patterns: floats compare exactly, NaN included).
bool OtherValuesDiffer(const OfpsSettingsValues &a, const OfpsSettingsValues &b) {
    if (a.count != b.count) return true;
    for (std::uint32_t i = 0; i < a.count && i < OFPS_SETTINGS_MAX; ++i)
        if (i != OFPS_SET_MENU_MODE && a.v[i].i != b.v[i].i) return true;
    return false;
}
} // namespace

void MenuSettingsChanged(const OfpsSettingsValues &values) {
    if (values.count > OFPS_SET_MENU_MODE)
        g_menuMode.store(values.v[OFPS_SET_MENU_MODE].i != 0, std::memory_order_relaxed);
    if (values.count > OFPS_SET_TEMPORAL_MODE) { // 2 (the hidden centre-every-frame variant) is a sync cadence too
        const int mode = values.v[OFPS_SET_TEMPORAL_MODE].i;
        g_temporalMode.store(mode == 2 ? 1 : mode, std::memory_order_relaxed);
    }
    std::lock_guard lock(g_lastMutex);
    if (g_haveLast && OtherValuesDiffer(g_last, values)) g_epoch.fetch_add(1, std::memory_order_acq_rel);
    g_last = values;
    g_haveLast = true;
}

bool MenuModeOn() { return g_menuMode.load(std::memory_order_relaxed); }

int MenuTemporalMode() { return g_temporalMode.load(std::memory_order_relaxed); }

std::uint64_t MenuSettingsEpoch() { return g_epoch.load(std::memory_order_acquire); }

} // namespace ofps::reshade
