#pragma once

#include "peripheral_warp/addon_api.h"
#include "peripheral_warp/input_v2.h"

namespace pw {

inline constexpr std::uint32_t kAddonApiVersionV2 = 2;

struct ResourceViewV2 {
    std::uint64_t handle;
    ResourceFormat typedFormat;
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t reserved[2];
};

// A universal, explicitly described DLSS-style input. The producer validates
// every typed view and never infers format or semantics from a resource name.
struct FrameInputV2 {
    std::uint32_t structSize;
    std::uint32_t version;
    std::uint64_t frameId;
    std::uint64_t generation;
    Backend backend;
    std::uint32_t flags;
    std::uint64_t runtime;
    std::uint64_t device;
    std::uint64_t commandList;
    ResourceViewV2 color;
    ResourceViewV2 depth;
    ResourceViewV2 motion;
    ResourceViewV2 confidence;
    InputDescriptionV2 description;
    std::uint32_t reserved[8];
};

struct PackedFrameV2 {
    std::uint32_t structSize;
    std::uint32_t version;
    PackedFrameV1 canonical;
    DepthConvention depthConvention;
    ColorEncoding colorEncoding;
    float jitterX;
    float jitterY;
    std::uint32_t inputFlags;
    std::uint32_t reserved[7];
};

using ConsumerCallbackV2 = void (PW_CALL *)(const PackedFrameV2 *frame, void *userData);

struct ConsumerRegistrationV2 {
    std::uint32_t structSize;
    std::uint32_t version;
    ConsumerCallbackV2 callback;
    void *userData;
    std::uint32_t reserved[8];
};

using RegisterConsumerFnV2 = Status (PW_CALL *)(const ConsumerRegistrationV2 *, std::uint64_t *);
using SubmitInputFnV2 = Status (PW_CALL *)(const FrameInputV2 *);
using PublishPackedFrameFnV2 = Status (PW_CALL *)(const PackedFrameV2 *);

struct AddonApiV2 {
    std::uint32_t structSize;
    std::uint32_t version;
    RegisterConsumerFnV2 registerConsumer;
    UnregisterConsumerFnV1 unregisterConsumer;
    GetConfigFnV1 getConfig;
    SetConfigFnV1 setConfig;
    GetLayoutFnV1 getLayout;
    SubmitInputFnV2 submitInput;
    PublishPackedFrameFnV2 publishPackedFrame;
};

static_assert(std::is_standard_layout_v<ResourceViewV2> &&
              std::is_trivially_copyable_v<ResourceViewV2>);
static_assert(std::is_standard_layout_v<FrameInputV2> &&
              std::is_trivially_copyable_v<FrameInputV2>);
static_assert(std::is_standard_layout_v<PackedFrameV2> &&
              std::is_trivially_copyable_v<PackedFrameV2>);
static_assert(std::is_standard_layout_v<ConsumerRegistrationV2> &&
              std::is_trivially_copyable_v<ConsumerRegistrationV2>);
static_assert(std::is_standard_layout_v<AddonApiV2> &&
              std::is_trivially_copyable_v<AddonApiV2>);

} // namespace pw

PW_ADDON_EXPORT const pw::AddonApiV2 *PW_CALL PeripheralWarpGetApiV2(
    std::uint32_t requestedVersion);
