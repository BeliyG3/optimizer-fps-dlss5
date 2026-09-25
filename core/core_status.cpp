#include "core/core_impl.h"
#include <algorithm>
#include <cstring>

namespace ofps::core {
namespace {
void EventCpp(IOfpsHost *host, OfpsEvent kind, const OfpsEventData *data) {
    try {
        host->OnEvent(kind, data);
    } catch (...) {
    }
}
void EventGuarded(IOfpsHost *host, OfpsEvent kind, const OfpsEventData *data) {
    __try {
        EventCpp(host, kind, data);
    } __except (gpu::RecordCrash(GetExceptionInformation(), StageModel, Ctx().crash)) {
    }
}
void LogCpp(IOfpsHost *host, OfpsLogLevel level, const char *text) {
    try {
        host->Log(level, text);
    } catch (...) {
    }
}
void LogGuarded(IOfpsHost *host, OfpsLogLevel level, const char *text) {
    __try {
        LogCpp(host, level, text);
    } __except (gpu::RecordCrash(GetExceptionInformation(), StageModel, Ctx().crash)) {
    }
}
} // namespace
void Core::ReportAbiException() noexcept {
    // Called after stack unwinding releases the entry-point lock. Never re-enter a callback.
    try {
        if (InCallback()) return;
        std::unique_lock lock(Ctx().mutex, std::try_to_lock);
        if (!lock.owns_lock() || hosts_.empty()) return;
        static bool reported = false;
        if (reported) return;
        reported = true;
        LogAll(OFPS_LOG_ERROR, "Optimizer FPS core: exception at ABI boundary");
    } catch (...) {
    }
}
void Core::Status(OfpsStatus *out) try {
    if (InCallback() || !out || out->size < sizeof(uint32_t))
        return;
    std::lock_guard lock(Ctx().mutex);
    FillStatus(Ctx().status, {directHost_ ? 1u : 0u, deviceRemoved_ ? 1u : 0u}, &statusStrings_, out);
} catch (...) {
    ReportAbiException();
}
uint32_t Core::StatusLines(OfpsStatusRow *rows, uint32_t capacity) try {
    if (InCallback() || !rows || !capacity)
        return 0;
    std::lock_guard lock(Ctx().mutex);
    const auto count = std::min(capacity, BuildStatusLines(Ctx().status, Ctx().temporal, &statusLines_));
    std::copy_n(statusLines_.rows, count, rows);
    return count;
} catch (...) {
    ReportAbiException();
    return 0; // This ABI method returns a row count, not an OfpsResult.
}
void Core::LayoutPreview(OfpsLayoutPreview *out) try {
    if (InCallback() || !out || out->size < sizeof(uint32_t))
        return;
    std::lock_guard lock(Ctx().mutex);
    OfpsLayoutPreview full{};
    full.size = sizeof(full);
    BuildLayoutPreview(settings_.config, Ctx().status.nativeWidth, Ctx().status.nativeHeight, &full);
    const auto size = std::min<uint32_t>(out->size, sizeof(full));
    std::memcpy(out, &full, size);
    out->size = size;
} catch (...) {
    ReportAbiException();
}
void Core::Emit(OfpsEvent kind, const OfpsEventData &data) {
    const bool previous = inCallback_;
    inCallback_ = true;
    for (const auto &entry : hosts_)
        EventGuarded(entry.host, kind, &data);
    inCallback_ = previous;
    if (!inCallback_)
        FlushRetirements();
}
void Core::LogAll(OfpsLogLevel level, const char *text) {
    const bool previous = inCallback_;
    inCallback_ = true;
    for (const auto &entry : hosts_)
        LogGuarded(entry.host, level, text);
    inCallback_ = previous;
}
void Core::LogSink(bool warning, const char *message) {
    CoreInstance().LogAll(warning ? OFPS_LOG_WARN : OFPS_LOG_INFO, message);
}
bool Core::DeviceRemovedNow() {
    auto *device = Ctx().warpDevice.load();
    return device && device->GetDeviceRemovedReason() != S_OK;
}
void Core::SetHostCaps(IOfpsHost *host, const OfpsHostCaps *caps) {
    if (InCallback() || !caps || caps->size < sizeof(*caps))
        return;
    std::lock_guard lock(Ctx().mutex);
    for (auto &entry : hosts_)
        if (entry.host == host) {
            entry.caps = *caps;
            return;
        }
}
void Core::SetDirectHost(IOfpsHost *host, uint32_t on) {
    if (InCallback())
        return;
    std::lock_guard lock(Ctx().mutex);
    if (std::none_of(hosts_.begin(), hosts_.end(), [host](const auto &e) { return e.host == host; }))
        return;
    auto *next = on ? host : directHost_ == host ? nullptr : directHost_;
    if (next == directHost_)
        return;
    directHost_ = next;
    const uint32_t active = directHost_ ? 1u : 0u;
    Emit(OFPS_EVENT_DIRECT_HOST_CHANGED, {sizeof(OfpsEventData), &active, nullptr, nullptr});
}
void Core::UnregisterHost(IOfpsHost *host) {
    if (InCallback())
        return;
    std::lock_guard lock(Ctx().mutex);
    std::erase_if(hosts_, [host](const auto &e) { return e.host == host; });
    if (directHost_ == host) {
        directHost_ = nullptr;
        const uint32_t active = 0;
        Emit(OFPS_EVENT_DIRECT_HOST_CHANGED, {sizeof(OfpsEventData), &active, nullptr, nullptr});
    }
}
} // namespace ofps::core
