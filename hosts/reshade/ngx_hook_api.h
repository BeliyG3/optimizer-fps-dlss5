#pragma once
#include <cstdint>
namespace ofps::reshade {
struct HookStatus {
    bool moduleFound = false;
    bool hooked = false;
    std::uint32_t hookAttempts = 0;
    int floatGetterSlot = -1;
    char reason[512] = {};
};
void SetHookReason(const char *text);
void Poll();
void SetEnabled(bool enabled);
void SetSafeMode(bool safe);
bool SafeMode();
HookStatus GetHookStatus();
void *CurrentParams();
void ForgetModelHost(void *hostHandle);
} // namespace ofps::reshade
