#include "core/warp/path_policy.h"

namespace ofps::core::warp {

PackDecision DecidePack(RequestedPath request, const PackSupport& s) noexcept {
    if (request == RequestedPath::Pixel)
        return {PackPath::Pixel, PathReason::ForcedPixel};
    PathReason failure = PathReason::None;
    if (!s.hostDirect && !s.privateCompute) failure = PathReason::UnsupportedList;
    else if (!s.sourcesValid) failure = PathReason::InvalidSource;
    else if (!s.shadersLoaded) failure = PathReason::MissingShader;
    else if (!s.colorTypedStore) failure = PathReason::MissingTypedColor;
    else if (!s.depthTypedStore) failure = PathReason::MissingTypedDepth;
    else if (!s.motionTypedStore) failure = PathReason::MissingTypedMotion;
    if (failure == PathReason::None) return {PackPath::Compute, PathReason::None};
    if (request == RequestedPath::Compute) return {PackPath::None, failure};
    if (s.hostDirect) return {PackPath::Pixel, failure};
    return {PackPath::None, failure};
}

UnpackDecision DecideUnpack(const UnpackSupport& s) noexcept {
    if (!s.answerValid) return {UnpackPath::None, PathReason::InvalidAnswer};
    const bool direct = s.outputTypedStore && s.outputAllowsUav &&
        s.outputSubresourceZero && s.outputRegionFits &&
        s.outputSingleSample2D && !s.temporalNeedsNativeRead;
    if (direct) return {UnpackPath::DirectUav, PathReason::None};
    if (s.intermediateTypedStore && s.copyFormatsCompatible && s.outputRegionFits)
        return {UnpackPath::CopyFromUav, PathReason::None};
    return {UnpackPath::None, PathReason::CannotCopy};
}

ModelListPath DecideModelList(const ModelListSupport& s) noexcept {
    if (s.hostList) return s.direct ? ModelListPath::HostDirect : ModelListPath::None;
    if (s.compute) return ModelListPath::PrivateCompute;
    if (s.direct) return ModelListPath::HostDirect;
    return ModelListPath::None;
}

const char* ReasonText(PathReason reason) noexcept {
    switch (reason) {
    case PathReason::None: return "ready";
    case PathReason::ForcedPixel: return "pixel path forced by DebugWarpPath";
    case PathReason::MissingShader: return "compute warp shader missing";
    case PathReason::MissingTypedColor: return "typed UAV store unsupported for packed color";
    case PathReason::MissingTypedDepth: return "typed UAV store unsupported for R32_FLOAT";
    case PathReason::MissingTypedMotion: return "typed UAV store unsupported for R16G16_FLOAT";
    case PathReason::InvalidSource: return "Pack source validation failed";
    case PathReason::UnsupportedList: return "command list type cannot record this path";
    case PathReason::CannotCopy: return "Unpack output cannot be written or copied";
    case PathReason::InvalidAnswer: return "model answer is not a valid Unpack source";
    }
    return "unknown warp path reason";
}

} // namespace ofps::core::warp
