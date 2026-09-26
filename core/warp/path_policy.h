#pragma once

#include <cstdint>

namespace ofps::core::warp {

enum class RequestedPath : std::uint32_t { Auto = 0, Compute = 1, Pixel = 2 };
enum class PackPath : std::uint32_t { None = 0, Compute = 1, Pixel = 2 };
enum class UnpackPath : std::uint32_t { None = 0, DirectUav = 1, CopyFromUav = 2 };
enum class ModelListPath : std::uint32_t { None = 0, HostDirect = 1, PrivateCompute = 2, HostCompute = 3 };

enum class PathReason : std::uint32_t {
    None = 0,
    ForcedPixel,
    MissingShader,
    MissingTypedColor,
    MissingTypedDepth,
    MissingTypedMotion,
    InvalidSource,
    UnsupportedList,
    CannotCopy,
    InvalidAnswer
};

struct PackSupport {
    bool colorTypedStore = false;
    bool depthTypedStore = false;
    bool motionTypedStore = false;
    bool shadersLoaded = false;
    bool sourcesValid = false;
    bool hostDirect = false;
    bool privateCompute = false;
    // The host's own list is COMPUTE (DLSS5-Reshade-AIO runs NR on async compute): only the compute
    // path can record there, so there is no pixel fallback.
    bool hostCompute = false;
};

struct PackDecision {
    PackPath path = PackPath::None;
    PathReason reason = PathReason::None;
};

struct UnpackSupport {
    bool answerValid = false;
    bool outputTypedStore = false;
    bool outputAllowsUav = false;
    bool outputSubresourceZero = false;
    bool outputRegionFits = false;
    bool outputSingleSample2D = false;
    bool copyFormatsCompatible = false;
    bool intermediateTypedStore = false;
    bool temporalNeedsNativeRead = false;
};

struct UnpackDecision {
    UnpackPath path = UnpackPath::None;
    PathReason reason = PathReason::None;
};

struct ModelListSupport {
    bool hostList = true;
    bool direct = false;
    bool compute = false;
};

[[nodiscard]] PackDecision DecidePack(RequestedPath request, const PackSupport& support) noexcept;
[[nodiscard]] UnpackDecision DecideUnpack(const UnpackSupport& support) noexcept;
[[nodiscard]] ModelListPath DecideModelList(const ModelListSupport& support) noexcept;
[[nodiscard]] const char* ReasonText(PathReason reason) noexcept;

} // namespace ofps::core::warp
