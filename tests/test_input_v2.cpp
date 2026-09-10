#include "peripheral_warp/addon_api.h"
#include "peripheral_warp/addon_api_v2.h"
#include "peripheral_warp/input_v2.h"

#include <cmath>
#include <cstdlib>
#include <iostream>

namespace {
int failures = 0;
void Check(bool condition, const char *message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}
}

int main()
{
    auto input = pw::DefaultInputDescriptionV2(3840, 2160);
    input.depthRect = {16, 8, 1920, 1080};
    input.motionRect = {32, 24, 1920, 1080};
    input.confidenceRect = {4, 2, 960, 540};
    input.motionScaleX = -3840.0f;
    input.motionScaleY = 2160.0f;
    input.motionDirection = pw::MotionDirection::PreviousToCurrent;
    input.depthConvention = pw::DepthConvention::Reversed;
    input.colorEncoding = pw::ColorEncoding::LinearHdr;
    input.flags = pw::InputFlagConfidenceValid | pw::InputFlagMotionJittered;
    input.jitterX = 0.25f;
    input.jitterY = -0.25f;

    const pw::InputResourceExtentsV2 extents{
        4096, 2304, 2048, 1152, 2048, 1152, 1024, 576};
    pw::ShaderInputConstantsV2 constants{};
    Check(pw::BuildShaderInputConstantsV2(input, extents, &constants) == pw::Status::Ok,
          "independent subrects and UV-to-native scales validate");
    Check(constants.colorRect[2] == 3840.0f && constants.depthRect[0] == 16.0f &&
              constants.motionRect[1] == 24.0f,
          "all source rectangles reach the fused shader constants");
    Check(constants.motionScaleX == -3840.0f && constants.motionScaleY == 2160.0f &&
              constants.motionDirectionSign == -1.0f,
          "motion units and direction remain explicit");
    Check(constants.colorDepthSize[0] == 4096.0f &&
              constants.motionConfidenceSize[3] == 576.0f,
          "independent resource extents reach the shader");

    auto invalid = input;
    invalid.colorEncoding = pw::ColorEncoding::Unknown;
    Check(pw::ValidateInputDescriptionV2(invalid) == pw::Status::InvalidFlags,
          "unknown colour encoding is rejected rather than guessed");
    invalid = input;
    invalid.motionRect = {2000, 0, 1920, 1080};
    Check(pw::BuildShaderInputConstantsV2(invalid, extents, &constants) ==
              pw::Status::InvalidDimensions,
          "a subrect outside its typed view is rejected");
    invalid = input;
    invalid.motionScaleX = 0.0f;
    Check(pw::ValidateInputDescriptionV2(invalid) == pw::Status::InvalidFlags,
          "zero or ambiguous motion scale is rejected");

    Check(pw::kAddonApiVersion == 1 && pw::kAddonApiVersionV2 == 2,
          "ABI v1 remains unchanged beside the new v2 export");
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
