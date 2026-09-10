#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

// The add-on's own C exports (stage 27.D2, moved from producer.cpp). The bench harness drives the
// add-on through these; a consumer add-on may use PeripheralWarpSetLayoutV1 the same way. They are
// declared nowhere in this tree: the callers resolve them with GetProcAddress, so this file has no
// header. Both must be called from the thread that presents (the overlay's thread).

#include "addon_context.h"
#include "config_store.h"
#include "layout_bridge.h"
#include "../ngx_hook.h"

#include "peripheral_warp/layout_bridge_v1.h"
#include "peripheral_warp/types_v2.h"

#include <cstdint>

// Programmatic layout change, equivalent to editing the overlay: the seven layout fields of the
// state are applied over the current configuration (flags are kept), forwarded to a linked
// consumer, persisted to ReShade.ini and picked up by the NGX interposer on its next evaluate.
// Call it from the thread that presents (the overlay's thread); returns a
// PeripheralWarpLayoutBridgeStatusV1 value, NotReady meaning "retry on a later frame".
// Programmatic temporal-mode change (the bench uses it): mode 0..2, full pass every `every` frames.
extern "C" __declspec(dllexport) std::uint32_t PeripheralWarpSetTemporalV1(std::uint32_t mode, std::uint32_t every)
{
    if (mode > 3 || mode == 2 || every < (mode == 3 ? 1u : 2u) || every > 8) return PeripheralWarpLayoutBridge_InvalidArgument; // 3 = background model
    pw_addon::State().temporal.mode = static_cast<int>(mode);
    pw_addon::State().temporal.every = static_cast<int>(every);
    pw_ngx::SetTemporal(pw_addon::State().temporal);
    return PeripheralWarpLayoutBridge_Ok;
}

extern "C" __declspec(dllexport) std::uint32_t PeripheralWarpSetLayoutV1(const PeripheralWarpLayoutStateV1 *state)
{
    if (state == nullptr || state->structSize < sizeof(PeripheralWarpLayoutStateV1))
        return PeripheralWarpLayoutBridge_InvalidArgument;
    pw::ConfigV2 config = pw_addon::ConfigForNgxHook();
    pw_addon::FromBridgeState(*state, config);
    if (pw::ValidateConfig(config) != pw::Status::Ok) return PeripheralWarpLayoutBridge_InvalidArgument;
    const pw::Status status = pw_addon::ApplyConfig(config);
    if (status == pw::Status::Ok) return pw_addon::BridgeLayoutRejected() ? PeripheralWarpLayoutBridge_Rejected : PeripheralWarpLayoutBridge_Ok;
    if (status == pw::Status::NotReady) return PeripheralWarpLayoutBridge_NotReady;
    return PeripheralWarpLayoutBridge_InvalidArgument;
}
