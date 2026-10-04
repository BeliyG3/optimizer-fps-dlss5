#pragma once
#include <cstdint>
#include <functional>
struct IOfpsFeature;
namespace ofps::reshade {
class ModelHostNgx;
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
// Menu mode, stage 3 (present thread, under the menu pipeline's lock): runs `call` with the feature behind hostHandle and
// its model host while no host evaluate or release holds them (correction C4: the feature-call lock is held from the
// lookup through the whole call, so a release waits for it and the call never sees a released feature). False when a
// host evaluate or release is in progress, or the feature is gone or released; never blocks.
bool WithFeatureForMenu(void *hostHandle, const std::function<bool(IOfpsFeature &, ModelHostNgx &)> &call);
} // namespace ofps::reshade
