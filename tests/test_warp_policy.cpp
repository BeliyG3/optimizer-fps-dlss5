#include "core/warp/path_policy.h"

#include <cstdio>
#include <cstdlib>

using namespace ofps::core::warp;

static void Check(bool value, const char* name) {
    if (!value) {
        std::fprintf(stderr, "FAIL %s\n", name);
        std::exit(1);
    }
}

int main() {
    PackSupport ready{};
    ready.colorTypedStore = true;
    ready.depthTypedStore = true;
    ready.motionTypedStore = true;
    ready.shadersLoaded = true;
    ready.sourcesValid = true;
    ready.hostDirect = true;
    Check(DecidePack(RequestedPath::Auto, ready).path == PackPath::Compute, "auto compute");
    Check(DecidePack(RequestedPath::Compute, ready).path == PackPath::Compute, "forced compute");
    Check(DecidePack(RequestedPath::Pixel, ready).path == PackPath::Pixel, "forced pixel");

    ready.motionTypedStore = false;
    auto choice = DecidePack(RequestedPath::Auto, ready);
    Check(choice.path == PackPath::Pixel && choice.reason == PathReason::MissingTypedMotion,
          "auto fallback identifies motion format");
    choice = DecidePack(RequestedPath::Compute, ready);
    Check(choice.path == PackPath::None && choice.reason == PathReason::MissingTypedMotion,
          "forced compute refuses unsupported format");
    ready.motionTypedStore = true;

    ready.hostDirect = false;
    ready.privateCompute = true;
    Check(DecidePack(RequestedPath::Auto, ready).path == PackPath::Compute,
          "private compute list");
    ready.shadersLoaded = false;
    Check(DecidePack(RequestedPath::Auto, ready).path == PackPath::None,
          "no pixel draw on compute list");

    UnpackSupport out{};
    out.answerValid = true;
    out.outputTypedStore = true;
    out.outputAllowsUav = true;
    out.outputSubresourceZero = true;
    out.outputRegionFits = true;
    out.outputSingleSample2D = true;
    out.copyFormatsCompatible = true;
    out.intermediateTypedStore = true;
    Check(DecideUnpack(out).path == UnpackPath::DirectUav, "direct output");
    out.outputAllowsUav = false;
    Check(DecideUnpack(out).path == UnpackPath::CopyFromUav, "intermediate output");
    out.outputAllowsUav = true;
    out.temporalNeedsNativeRead = true;
    Check(DecideUnpack(out).path == UnpackPath::CopyFromUav, "temporal residual");
    out.copyFormatsCompatible = false;
    Check(DecideUnpack(out).path == UnpackPath::None, "incompatible copy");
    out.answerValid = false;
    Check(DecideUnpack(out).reason == PathReason::InvalidAnswer, "codec answer validation");

    Check(DecideModelList({true, true, false}) == ModelListPath::HostDirect,
          "host direct list");
    Check(DecideModelList({true, false, true}) == ModelListPath::None,
          "host compute rejected");
    Check(DecideModelList({false, false, true}) == ModelListPath::PrivateCompute,
          "private compute accepted");
    Check(DecideModelList({false, false, false}) == ModelListPath::None,
          "copy and bundle rejected");
    std::puts("warp policy: pass");
    return 0;
}
