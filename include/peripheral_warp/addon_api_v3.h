#pragma once

#include "peripheral_warp/addon_api_v2.h"
#include "peripheral_warp/types_v2.h"

namespace pw {

inline constexpr std::uint32_t kAddonApiVersionV3 = 3;

struct PackedFrameV3 {
    std::uint32_t structSize;
    std::uint32_t version;
    std::uint64_t frameId;
    std::uint64_t layoutGeneration;
    LayoutV2 layout;
    Backend backend;
    ResourceFormat colorFormat;
    ResourceFormat depthFormat;
    ResourceFormat motionFormat;
    ResourceFormat confidenceFormat;
    std::uint32_t flags;
    std::uint64_t runtime;
    std::uint64_t device;
    std::uint64_t commandList;
    std::uint64_t colorResourceView;
    std::uint64_t depthResourceView;
    std::uint64_t motionResourceView;
    std::uint64_t confidenceResourceView;
    DepthConvention depthConvention;
    ColorEncoding colorEncoding;
    float jitterX;
    float jitterY;
    std::uint32_t inputFlags;
    std::uint32_t reserved[7];
};

using ConsumerCallbackV3 = void (PW_CALL *)(const PackedFrameV3 *frame, void *userData);

struct ConsumerRegistrationV3 {
    std::uint32_t structSize;
    std::uint32_t version;
    ConsumerCallbackV3 callback;
    void *userData;
    std::uint32_t reserved[8];
};

using RegisterConsumerFnV3 = Status (PW_CALL *)(const ConsumerRegistrationV3 *, std::uint64_t *);
using GetConfigFnV2 = Status (PW_CALL *)(ConfigV2 *);
using SetConfigFnV2 = Status (PW_CALL *)(const ConfigV2 *);
using GetLayoutFnV2 = Status (PW_CALL *)(std::uint64_t, LayoutV2 *, std::uint64_t *);
using PublishPackedFrameFnV3 = Status (PW_CALL *)(const PackedFrameV3 *);

// InputDescriptionV2 remains the canonical explicit input contract. API v3 only
// widens configuration, layout, and packed-frame publication around that input.
struct AddonApiV3 {
    std::uint32_t structSize;
    std::uint32_t version;
    RegisterConsumerFnV3 registerConsumer;
    UnregisterConsumerFnV1 unregisterConsumer;
    GetConfigFnV2 getConfig;
    SetConfigFnV2 setConfig;
    GetLayoutFnV2 getLayout;
    SubmitInputFnV2 submitInput;
    PublishPackedFrameFnV3 publishPackedFrame;
};

static_assert(sizeof(PackedFrameV3) == 312);
static_assert(sizeof(ConsumerRegistrationV3) == (sizeof(void *) == 8 ? 56 : 48));
static_assert(sizeof(AddonApiV3) == (sizeof(void *) == 8 ? 64 : 36));
static_assert(offsetof(PackedFrameV3, frameId) == 8);
static_assert(offsetof(PackedFrameV3, layout) == 24);
static_assert(offsetof(PackedFrameV3, backend) == 184);
static_assert(offsetof(PackedFrameV3, flags) == 204);
static_assert(offsetof(PackedFrameV3, runtime) == 208);
static_assert(offsetof(PackedFrameV3, colorResourceView) == 232);
static_assert(offsetof(PackedFrameV3, depthConvention) == 264);
static_assert(offsetof(PackedFrameV3, reserved) == 284);
static_assert(offsetof(ConsumerRegistrationV3, callback) == 8);
static_assert(offsetof(ConsumerRegistrationV3, userData) == (sizeof(void *) == 8 ? 16 : 12));
static_assert(offsetof(AddonApiV3, registerConsumer) == 8);
static_assert(offsetof(AddonApiV3, publishPackedFrame) == (sizeof(void *) == 8 ? 56 : 32));
static_assert(std::is_standard_layout_v<PackedFrameV3> &&
              std::is_trivially_copyable_v<PackedFrameV3>);
static_assert(std::is_standard_layout_v<ConsumerRegistrationV3> &&
              std::is_trivially_copyable_v<ConsumerRegistrationV3>);
static_assert(std::is_standard_layout_v<AddonApiV3> &&
              std::is_trivially_copyable_v<AddonApiV3>);

} // namespace pw

PW_ADDON_EXPORT const pw::AddonApiV3 *PW_CALL PeripheralWarpGetApiV3(
    std::uint32_t requestedVersion);
