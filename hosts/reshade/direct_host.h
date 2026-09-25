#pragma once
#include <windows.h>
#include <mutex>
namespace ofps::reshade {
// Producer contract: Local\OptimizerFpsDirectHost_<pid> is manual-reset.
// The producer calls SetEvent before the first NR create; only a signaled event
// activates the probe. Auto-reset is forbidden because probing consumes its signal.
class DirectHostProbe final {
public:
    explicit DirectHostProbe(ULONGLONG intervalMs = 250) : intervalMs_(intervalMs) {}
    DirectHostProbe(const DirectHostProbe &) = delete;
    DirectHostProbe &operator=(const DirectHostProbe &) = delete;
    ~DirectHostProbe();
    bool Active();
private:
    std::mutex mutex_;
    HANDLE event_ = nullptr;
    const ULONGLONG intervalMs_;
    ULONGLONG lastAttempt_ = 0;
    bool attempted_ = false;
};
bool DirectHostActive();
} // namespace ofps::reshade
