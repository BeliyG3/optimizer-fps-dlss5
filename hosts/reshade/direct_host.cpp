#include "hosts/reshade/direct_host.h"
#include <cstdio>
namespace ofps::reshade {
DirectHostProbe::~DirectHostProbe() { if (event_) CloseHandle(event_); }
bool DirectHostProbe::Active() {
    std::lock_guard lock(mutex_);
    if (!event_) {
        const ULONGLONG now = GetTickCount64();
        if (attempted_ && now - lastAttempt_ < intervalMs_) return false;
        attempted_ = true;
        lastAttempt_ = now;
        wchar_t name[96];
        swprintf_s(name, L"Local\\OptimizerFpsDirectHost_%lu", GetCurrentProcessId());
        event_ = OpenEventW(SYNCHRONIZE, FALSE, name);
        if (!event_) return false;
    }
    const DWORD result = WaitForSingleObject(event_, 0);
    if (result != WAIT_OBJECT_0) { CloseHandle(event_); event_ = nullptr; }
    return result == WAIT_OBJECT_0;
}
bool DirectHostActive() {
    // Detours must observe an event created after a prior missed lookup on the next entry.
    static DirectHostProbe probe(0);
    return probe.Active();
}
} // namespace ofps::reshade
