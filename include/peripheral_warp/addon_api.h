#pragma once

#include "peripheral_warp/types.h"

#if defined(_WIN32)
#define PW_CALL __cdecl
#else
#define PW_CALL
#endif

namespace pw {

inline constexpr std::uint32_t kAddonApiVersion = 1;

enum FrameFlags : std::uint32_t {
    FrameFlagNone = 0,
    FrameFlagColorValid = 1u << 0,
    FrameFlagDepthValid = 1u << 1,
    FrameFlagMotionValid = 1u << 2,
    FrameFlagConfidenceValid = 1u << 3,
};

enum class Backend : std::uint32_t {
    Unknown = 0,
    D3D11 = 1,
    D3D12 = 2,
    Vulkan = 3,
};

enum class ResourceFormat : std::uint32_t {
    Unknown = 0,
    Rgba8Unorm = 1,
    Rgba8UnormSrgb = 2,
    Bgra8Unorm = 3,
    Bgra8UnormSrgb = 4,
    Rgba16Float = 5,
    R32Float = 6,
    Rg16Float = 7,
    R16Float = 8,
    R16Unorm = 9,
    R24UnormX8Typeless = 10,
    R32FloatX8X24Typeless = 11,
    Rg32Float = 12,
    Rg16Snorm = 13,
    Rg16Unorm = 14,
    R8Unorm = 15,
    Rgb10A2Unorm = 16,
    Rg11B10Float = 17,
};

// Canonical, unwarped input. Motion is current-pixel -> previous-pixel in native
// pixel units. runtime/device/commandList are ReShade API object pointers encoded
// as integers; resource-view fields are reshade::api::resource_view::handle values.
// submitInput is synchronous; all objects and views must remain valid until return.
struct FrameInputV1 {
    std::uint32_t structSize;
    std::uint32_t version;
    std::uint64_t frameId;
    std::uint32_t nativeWidth;
    std::uint32_t nativeHeight;
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
    std::uint32_t reserved[8];
};

// Object and resource-view values use the same ReShade handle convention as
// FrameInputV1. The callback is synchronous; CPU handles are valid only for its
// duration. A consumer must leave packed resources in shader_resource usage and
// restore any command-list state it changes before returning. GPU-referenced
// resources owned by the consumer must stay alive until that queue work retires.
struct PackedFrameV1 {
    std::uint32_t structSize;
    std::uint32_t version;
    std::uint64_t frameId;
    std::uint64_t layoutGeneration;
    LayoutV1 layout;
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
    std::uint32_t reserved[8];
};

// Callbacks must not throw across this C ABI. Registration, mutation, submit and
// recursive publication calls are rejected while a callback is being dispatched;
// getConfig and getLayout remain safe read-only queries.
using ConsumerCallbackV1 = void (PW_CALL *)(const PackedFrameV1 *frame, void *userData);

struct ConsumerRegistrationV1 {
    std::uint32_t structSize;
    std::uint32_t version;
    ConsumerCallbackV1 callback;
    void *userData;
    std::uint32_t reserved[8];
};

using RegisterConsumerFnV1 = Status (PW_CALL *)(const ConsumerRegistrationV1 *, std::uint64_t *);
using UnregisterConsumerFnV1 = Status (PW_CALL *)(std::uint64_t);
using GetConfigFnV1 = Status (PW_CALL *)(ConfigV1 *);
using SetConfigFnV1 = Status (PW_CALL *)(const ConfigV1 *);
using GetLayoutFnV1 = Status (PW_CALL *)(std::uint64_t, LayoutV1 *, std::uint64_t *);
using SubmitInputFnV1 = Status (PW_CALL *)(const FrameInputV1 *);
using PublishPackedFrameFnV1 = Status (PW_CALL *)(const PackedFrameV1 *);

struct AddonApiV1 {
    std::uint32_t structSize;
    std::uint32_t version;
    RegisterConsumerFnV1 registerConsumer;
    UnregisterConsumerFnV1 unregisterConsumer;
    GetConfigFnV1 getConfig;
    SetConfigFnV1 setConfig;
    GetLayoutFnV1 getLayout;
    // Packs a canonical input through the matching ReShade runtime and publishes the
    // result before returning. This semantic-rebinding path is D3D11-only. D3D12
    // callers use the native adapter; Vulkan uses the automatic in-process
    // QuantMotion path. Call only on the active ReShade render thread with its
    // event-supplied command list while that reshade_begin_effects pass is active.
    SubmitInputFnV1 submitInput;
    // Publishes output already packed by a direct D3D11/D3D12 adapter. No GPU work is
    // performed; callbacks run synchronously on the caller's command-list lifetime.
    // The frame layout and generation must exactly match getLayout for the runtime.
    // Call during the matching active reshade_begin_effects pass with that pass's
    // command list.
    // Registration, unregistration and publishing must be serialized on that same
    // render thread; unregisterConsumer returns NotReady during active dispatch.
    PublishPackedFrameFnV1 publishPackedFrame;
};

static_assert(sizeof(FrameInputV1) == 136);
static_assert(sizeof(PackedFrameV1) == 216);
static_assert(sizeof(ConsumerRegistrationV1) == (sizeof(void *) == 8 ? 56 : 48));
static_assert(sizeof(AddonApiV1) == (sizeof(void *) == 8 ? 64 : 36));
static_assert(offsetof(FrameInputV1, frameId) == 8);
static_assert(offsetof(FrameInputV1, backend) == 24);
static_assert(offsetof(FrameInputV1, flags) == 44);
static_assert(offsetof(FrameInputV1, runtime) == 48);
static_assert(offsetof(FrameInputV1, colorResourceView) == 72);
static_assert(offsetof(FrameInputV1, reserved) == 104);
static_assert(offsetof(PackedFrameV1, frameId) == 8);
static_assert(offsetof(PackedFrameV1, layout) == 24);
static_assert(offsetof(PackedFrameV1, backend) == 104);
static_assert(offsetof(PackedFrameV1, flags) == 124);
static_assert(offsetof(PackedFrameV1, runtime) == 128);
static_assert(offsetof(PackedFrameV1, colorResourceView) == 152);
static_assert(offsetof(PackedFrameV1, reserved) == 184);
static_assert(offsetof(ConsumerRegistrationV1, callback) == 8);
static_assert(offsetof(ConsumerRegistrationV1, userData) == (sizeof(void *) == 8 ? 16 : 12));
static_assert(offsetof(ConsumerRegistrationV1, reserved) == (sizeof(void *) == 8 ? 24 : 16));
static_assert(offsetof(AddonApiV1, registerConsumer) == 8);
static_assert(offsetof(AddonApiV1, publishPackedFrame) == (sizeof(void *) == 8 ? 56 : 32));
static_assert(std::is_standard_layout_v<FrameInputV1> && std::is_trivially_copyable_v<FrameInputV1>);
static_assert(std::is_standard_layout_v<PackedFrameV1> && std::is_trivially_copyable_v<PackedFrameV1>);
static_assert(std::is_standard_layout_v<ConsumerRegistrationV1> && std::is_trivially_copyable_v<ConsumerRegistrationV1>);
static_assert(std::is_standard_layout_v<AddonApiV1> && std::is_trivially_copyable_v<AddonApiV1>);

} // namespace pw

#if defined(_WIN32)
#define PW_ADDON_EXPORT extern "C" __declspec(dllexport)
#else
#define PW_ADDON_EXPORT extern "C" __attribute__((visibility("default")))
#endif

PW_ADDON_EXPORT const pw::AddonApiV1 *PW_CALL PeripheralWarpGetApi(std::uint32_t requestedVersion);
