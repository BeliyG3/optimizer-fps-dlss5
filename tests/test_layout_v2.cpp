#include "optimizer_fps/addon_api.h"
#include "optimizer_fps/addon_api_v2.h"
#include "optimizer_fps/addon_api_v3.h"
#include "optimizer_fps/math.h"
#include "optimizer_fps/types_v2.h"

#include <algorithm>
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

bool Near(float lhs, float rhs, float tolerance = 1.0e-4f)
{
    return std::abs(lhs - rhs) <= tolerance;
}

void TestAbi()
{
    Check(ofps::sdk::kAbiVersion == 1 && ofps::sdk::kAbiVersionV2 == 2,
          "layout ABI versions remain distinct");
    Check(ofps::sdk::kAddonApiVersion == 1 && ofps::sdk::kAddonApiVersionV2 == 2 &&
              ofps::sdk::kAddonApiVersionV3 == 3,
          "add-on API v1/v2 remain unchanged beside v3");
    Check(sizeof(ofps::sdk::ConfigV1) == 64 && sizeof(ofps::sdk::LayoutV1) == 80,
          "legacy configuration ABI sizes remain unchanged");
    Check(sizeof(ofps::sdk::ConfigV2) == 64 && sizeof(ofps::sdk::LayoutV2) == 160,
          "v2 configuration ABI sizes are canonical");
    Check(sizeof(ofps::sdk::PackedFrameV1) == 216 && sizeof(ofps::sdk::PackedFrameV3) == 312,
          "packed-frame ABI sizes are canonical");
}

void TestDefaultAndGlobalScale()
{
    const ofps::sdk::ConfigV2 config = ofps::sdk::DefaultConfigV2();
    ofps::sdk::LayoutV2 layout{};
    Check(ofps::sdk::BuildLayout(config, 3840, 2160, &layout) == ofps::sdk::Status::Ok,
          "default v2 layout builds");
    Check(layout.rawWorkWidth == 3456 && layout.rawWorkHeight == 1944,
          "default raw Work is 80->90 at 4K");
    Check(layout.workWidth == 3456 && layout.workHeight == 1944,
          "Global 100 keeps raw and actual Work equal");
    Check(Near(layout.pixelPercent, 81.0f) && layout.diagnosticFlags == 0,
          "default pixel ratio is 81 percent without aggressive warning");

    auto scaled = config;
    scaled.globalScalePercent = ofps::sdk::MinimumGlobalScalePercent(scaled);
    Check(Near(scaled.globalScalePercent, 27.777778f, 1.0e-3f),
          "80->90 dynamic Global minimum is 27.7778 percent");
    Check(ofps::sdk::BuildLayout(scaled, 3840, 2160, &layout) == ofps::sdk::Status::Ok,
          "80->90 at the dynamic Global minimum builds");
    Check(layout.rawWorkWidth == 3456 && layout.rawWorkHeight == 1944 &&
              layout.workWidth == 960 && layout.workHeight == 540,
          "Global scale is fused directly into the final 960x540 Work extent");
    Check(Near(layout.pixelPercent, 6.25f),
          "minimum 4K NR input contains 6.25 percent of native pixels");
    Check((layout.diagnosticFlags & (ofps::sdk::LayoutDiagnosticAliasingRiskX |
                                     ofps::sdk::LayoutDiagnosticAliasingRiskY)) != 0,
          "Global downscale contributes to local aliasing diagnostics");
}

void TestAggressiveAndAsymmetricLayouts()
{
    auto config = ofps::sdk::DefaultConfigV2();
    config.xAxis = {20.0f, 25.0f};
    config.yAxis = {20.0f, 25.0f};
    ofps::sdk::LayoutV2 layout{};
    Check(ofps::sdk::BuildLayout(config, 3840, 2160, &layout) == ofps::sdk::Status::Ok,
          "20->25 Global 100 is accepted");
    Check(layout.workWidth == 960 && layout.workHeight == 540,
          "20->25 reaches the per-axis 25 percent floor");
    Check(Near(layout.compressionX, 0.0625f) &&
              Near(layout.maximumSourceFootprintX, 256.0f, 0.05f),
          "aggressive compression and expected footprint are exposed");
    Check((layout.diagnosticFlags & ofps::sdk::LayoutDiagnosticAggressivePeripheralX) != 0 &&
              (layout.diagnosticFlags & ofps::sdk::LayoutDiagnosticAggressivePeripheralY) != 0,
          "aggressive peripheral axes are reported independently");

    config.xAxis = {25.0f, 50.0f};
    config.yAxis = {25.0f, 50.0f};
    config.globalScalePercent = 50.0f;
    Check(ofps::sdk::BuildLayout(config, 3840, 2160, &layout) == ofps::sdk::Status::Ok,
          "25->50 Global 50 builds");
    Check(layout.rawWorkWidth == 1920 && layout.rawWorkHeight == 1080 &&
              layout.workWidth == 960 && layout.workHeight == 540,
          "raw boundary and actual NR dimensions remain distinct");

    config.xAxis = {70.0f, 80.0f};
    config.yAxis = {20.0f, 25.0f};
    config.globalScalePercent = 100.0f;
    Check(ofps::sdk::BuildLayout(config, 3840, 2160, &layout) == ofps::sdk::Status::Ok,
          "asymmetric X/Y configuration builds");
    Check(layout.rawWorkWidth == 3072 && layout.rawWorkHeight == 540 &&
              layout.workWidth == 3072 && layout.workHeight == 540,
          "asymmetric raw and actual dimensions are per-axis exact");
}

void TestValidation()
{
    auto config = ofps::sdk::DefaultConfigV2();
    config.xAxis.workPercent = 24.999f;
    Check(ofps::sdk::ValidateConfig(config) == ofps::sdk::Status::InvalidAxis,
          "Work below 25 percent is rejected");
    config = ofps::sdk::DefaultConfigV2();
    config.xAxis.centerPercent = config.xAxis.workPercent;
    Check(ofps::sdk::ValidateConfig(config) == ofps::sdk::Status::InvalidAxis,
          "Center equal to Work is rejected");
    config.xAxis.centerPercent = 95.0f;
    Check(ofps::sdk::ValidateConfig(config) == ofps::sdk::Status::InvalidAxis,
          "Center above Work is rejected");
    config = ofps::sdk::DefaultConfigV2();
    config.globalScalePercent = 27.7f;
    Check(ofps::sdk::ValidateConfig(config) == ofps::sdk::Status::InvalidAxis,
          "effective per-axis scale below 25 percent is rejected");
    config.globalScalePercent = ofps::sdk::MinimumGlobalScalePercent(config);
    Check(ofps::sdk::ValidateConfig(config) == ofps::sdk::Status::Ok,
          "the exact dynamic Global lower bound is accepted");

    ofps::sdk::ConfigV1 legacy = ofps::sdk::DefaultConfigV1();
    legacy.xAxis.workPercent = 85.0f;
    Check(ofps::sdk::ValidateConfig(legacy) == ofps::sdk::Status::InvalidAxis,
          "legacy v1 keeps its historical compression guard");

    legacy = ofps::sdk::DefaultConfigV1();
    ofps::sdk::ConfigV2 upgradedConfig{};
    Check(ofps::sdk::UpgradeConfig(legacy, &upgradedConfig) == ofps::sdk::Status::Ok &&
              Near(upgradedConfig.globalScalePercent, 100.0f),
          "legacy configuration upgrades with Global 100");
    ofps::sdk::ConfigV1 downgradedConfig{};
    Check(ofps::sdk::DowngradeConfig(upgradedConfig, &downgradedConfig) == ofps::sdk::Status::Ok &&
              downgradedConfig.xAxis.workPercent == legacy.xAxis.workPercent,
          "compatible v2 configuration round-trips to legacy ABI");

    ofps::sdk::LayoutV1 legacyLayout{};
    ofps::sdk::LayoutV2 upgradedLayout{};
    ofps::sdk::LayoutV1 downgradedLayout{};
    Check(ofps::sdk::BuildLayout(legacy, 3840, 2160, &legacyLayout) == ofps::sdk::Status::Ok &&
              ofps::sdk::UpgradeLayout(legacyLayout, &upgradedLayout) == ofps::sdk::Status::Ok &&
              ofps::sdk::DowngradeLayout(upgradedLayout, &downgradedLayout) == ofps::sdk::Status::Ok &&
              downgradedLayout.workWidth == legacyLayout.workWidth &&
              Near(downgradedLayout.compressionX, legacyLayout.compressionX),
          "compatible layouts bridge without changing legacy dimensions or math");

    ofps::sdk::LayoutV2 layout{};
    Check(ofps::sdk::BuildLayout(config, 3840, 2160, &layout) == ofps::sdk::Status::Ok &&
              ofps::sdk::ValidateLayout(layout) == ofps::sdk::Status::Ok,
          "canonical v2 layout validates");
    layout.rawWorkWidth += 2;
    Check(ofps::sdk::ValidateLayout(layout) == ofps::sdk::Status::InvalidDimensions,
          "layout validation rejects a forged raw Work extent");
}

void TestMathRoundTrip()
{
    auto config = ofps::sdk::DefaultConfigV2();
    config.globalScalePercent = 50.0f;
    ofps::sdk::LayoutV2 layout{};
    Check(ofps::sdk::BuildLayout(config, 3840, 2160, &layout) == ofps::sdk::Status::Ok,
          "scaled math layout builds");
    float maxPositionError = 0.0f;
    float maxMotionError = 0.0f;
    for (int y = 50; y < 2160; y += 173) {
        for (int x = 50; x < 3840; x += 211) {
            const ofps::sdk::Float2 native{static_cast<float>(x) + 0.5f,
                                    static_cast<float>(y) + 0.5f};
            const ofps::sdk::Float2 packed = ofps::sdk::PackPosition(native, layout);
            const ofps::sdk::Float2 restored = ofps::sdk::UnpackPosition(packed, layout);
            maxPositionError = std::max(
                maxPositionError,
                std::max(std::abs(restored.x - native.x),
                         std::abs(restored.y - native.y)));
            const ofps::sdk::Float2 motion{17.25f, -9.5f};
            const ofps::sdk::Float2 packedMotion = ofps::sdk::PackMotion(native, motion, layout);
            const ofps::sdk::Float2 restoredMotion = ofps::sdk::UnpackMotion(packed, packedMotion, layout);
            maxMotionError = std::max(
                maxMotionError,
                std::max(std::abs(restoredMotion.x - motion.x),
                         std::abs(restoredMotion.y - motion.y)));
        }
    }
    Check(maxPositionError < 0.01f, "v2 Pack/Unpack position round-trip stays subpixel");
    Check(maxMotionError < 0.05f, "v2 motion-vector round-trip stays below 0.05 px");

    const ofps::sdk::ShaderConstantsV2 constants = ofps::sdk::BuildShaderConstants(layout);
    Check(constants.workWidth == static_cast<float>(layout.workWidth) &&
              constants.workFractionX == layout.rawWorkFractionX,
          "fused shader constants combine actual extents with raw Warp fractions");
}

void TestCenterOffset()
{
    // Offset moves the 1:1 band; Work stays the same size; the wider periphery is compressed
    // harder and the narrower one never below 1:1.
    auto config = ofps::sdk::DefaultConfigV2();
    Check(Near(ofps::sdk::MaximumCenterOffsetPercentV2(80.0f), 9.5f),
          "80 percent centre allows an offset of up to 9.5 percent");
    config.centerOffsetXPercent = 6.0f;
    config.centerOffsetYPercent = -9.5f;
    Check(ofps::sdk::ValidateConfig(config) == ofps::sdk::Status::Ok, "offsets inside the limit are accepted");
    ofps::sdk::LayoutV2 layout{};
    Check(ofps::sdk::BuildLayout(config, 3840, 2160, &layout) == ofps::sdk::Status::Ok, "offset layout builds");
    Check(ofps::sdk::ValidateLayout(layout) == ofps::sdk::Status::Ok, "offset layout validates");
    Check(layout.workWidth == 3456 && layout.workHeight == 1944,
          "offset keeps the Work extent");
    // X: band centre at 1920 + 230.4 = 2150.4; peripheries 614.4 (left, wide) / 153.6 (right, narrow);
    // budget 3456 - 3072 = 384 -> the narrow right side gets all its 153.6 px (1:1), the wide
    // left side gets the remaining 230.4 px -> 0.375.
    Check(Near(layout.compressionXNeg, 0.375f, 1.0e-3f) && Near(layout.compressionXPos, 1.0f, 1.0e-3f),
          "X: the narrow side keeps 1:1, the wide side takes the remaining budget");
    Check(Near(layout.compressionX, 0.375f, 1.0e-3f),
          "the symmetric compression field reports the harder side");
    // Y: offset -9.5 puts the band 0.5 percent from the top: top periphery 10.8 px.
    Check(layout.compressionYNeg <= 1.0f + 1.0e-4f && layout.compressionYPos < layout.compressionYNeg,
          "Y: the near-edge side is not compressed, the far side is");

    float maxPositionError = 0.0f, maxMotionError = 0.0f;
    for (int y = 3; y < 2160; y += 131) {
        for (int x = 3; x < 3840; x += 157) {
            const ofps::sdk::Float2 native{static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f};
            const ofps::sdk::Float2 packed = ofps::sdk::PackPosition(native, layout);
            Check(packed.x >= -0.01f && packed.x <= static_cast<float>(layout.workWidth) + 0.01f &&
                      packed.y >= -0.01f && packed.y <= static_cast<float>(layout.workHeight) + 0.01f,
                  "offset Pack stays inside the Work frame");
            const ofps::sdk::Float2 restored = ofps::sdk::UnpackPosition(packed, layout);
            maxPositionError = std::max(maxPositionError,
                std::max(std::abs(restored.x - native.x), std::abs(restored.y - native.y)));
            const ofps::sdk::Float2 motion{11.5f, -7.25f};
            const ofps::sdk::Float2 packedMotion = ofps::sdk::PackMotion(native, motion, layout);
            const ofps::sdk::Float2 restoredMotion = ofps::sdk::UnpackMotion(packed, packedMotion, layout);
            maxMotionError = std::max(maxMotionError,
                std::max(std::abs(restoredMotion.x - motion.x), std::abs(restoredMotion.y - motion.y)));
        }
    }
    Check(maxPositionError < 0.01f, "offset Pack/Unpack round-trip stays subpixel");
    Check(maxMotionError < 0.05f, "offset motion round-trip stays below 0.05 px");
    // The band itself is 1:1: two native pixels inside it map to the same distance apart.
    const float bandCenterX = 1920.0f + 0.06f * 3840.0f;
    const ofps::sdk::Float2 a = ofps::sdk::PackPosition({bandCenterX - 100.0f, 1080.0f}, layout);
    const ofps::sdk::Float2 b = ofps::sdk::PackPosition({bandCenterX + 100.0f, 1080.0f}, layout);
    Check(Near(b.x - a.x, 200.0f, 1.0e-2f), "the offset centre band is still 1:1");
    // Frame corners map to the Work corners.
    const ofps::sdk::Float2 corner = ofps::sdk::PackPosition({3840.0f, 2160.0f}, layout);
    Check(Near(corner.x, 3456.0f, 1.0e-2f) && Near(corner.y, 1944.0f, 1.0e-2f),
          "the far corner maps to the Work corner");
    const ofps::sdk::ShaderConstantsV2 constants = ofps::sdk::BuildShaderConstants(layout);
    Check(Near(constants.bandCenterX, bandCenterX, 1.0e-2f) && Near(constants.sideCompressionPosX, layout.compressionXPos, 1.0e-5f),
          "shader constants carry the band centre and per-side compression");

    // Zero offset reproduces the symmetric layout exactly.
    auto symmetric = ofps::sdk::DefaultConfigV2();
    ofps::sdk::LayoutV2 symmetricLayout{};
    Check(ofps::sdk::BuildLayout(symmetric, 3840, 2160, &symmetricLayout) == ofps::sdk::Status::Ok &&
              Near(symmetricLayout.compressionXNeg, 0.5f) && Near(symmetricLayout.compressionXPos, 0.5f) &&
              Near(symmetricLayout.compressionX, 0.5f),
          "zero offset gives the classic symmetric compression");
    const ofps::sdk::Float2 mid = ofps::sdk::PackPosition({1920.0f, 1080.0f}, symmetricLayout);
    Check(Near(mid.x, 1728.0f, 1.0e-3f) && Near(mid.y, 972.0f, 1.0e-3f),
          "zero offset keeps the frame centre at the Work centre");

    // Fractional Center/Work/offset values must still translate the 1:1 band by whole texels,
    // otherwise Pack and Unpack sample between texels and blur the band.
    {
        auto fractional = ofps::sdk::DefaultConfigV2();
        fractional.xAxis = {57.1f, 84.5f};
        fractional.yAxis = {55.3f, 84.5f};
        fractional.centerOffsetXPercent = 1.111114f;
        fractional.centerOffsetYPercent = -5.5555553f;
        ofps::sdk::LayoutV2 snapped{};
        Check(ofps::sdk::BuildLayout(fractional, 3840, 2160, &snapped) == ofps::sdk::Status::Ok &&
                  ofps::sdk::ValidateLayout(snapped) == ofps::sdk::Status::Ok,
              "fractional layout builds and validates");
        const float bandX = 1920.0f + 0.01111114f * 3840.0f, bandY = 1080.0f - 0.055555553f * 2160.0f;
        // A texel centre inside the band must map to a texel centre (pure integer translation).
        const ofps::sdk::Float2 inBand = ofps::sdk::PackPosition({std::floor(bandX) + 100.5f, std::floor(bandY) - 40.5f}, snapped);
        const float fracX = inBand.x - std::floor(inBand.x), fracY = inBand.y - std::floor(inBand.y);
        Check(Near(fracX, 0.5f, 1.0e-3f) && Near(fracY, 0.5f, 1.0e-3f),
              "the 1:1 band is translated by whole texels for fractional percentages");
        Check(snapped.compressionXNeg <= 1.0f + 1.0e-4f && snapped.compressionXPos <= 1.0f + 1.0e-4f &&
                  snapped.compressionYNeg <= 1.0f + 1.0e-4f && snapped.compressionYPos <= 1.0f + 1.0e-4f,
              "snapping keeps every side at or below 1:1");
    }

    // Validation and the legacy bridge.
    config.centerOffsetXPercent = 9.6f;
    Check(ofps::sdk::ValidateConfig(config) == ofps::sdk::Status::InvalidAxis, "an offset beyond the limit is rejected");
    config.centerOffsetXPercent = 5.0f;
    config.centerOffsetYPercent = 0.0f;
    ofps::sdk::ConfigV1 legacy{};
    Check(ofps::sdk::DowngradeConfig(config, &legacy) == ofps::sdk::Status::InvalidAxis,
          "a configuration with an offset does not downgrade to v1");
    config.mode = ofps::sdk::WarpMode::Uniform;
    Check(ofps::sdk::ValidateConfig(config) == ofps::sdk::Status::Ok &&
              ofps::sdk::BuildLayout(config, 3840, 2160, &layout) == ofps::sdk::Status::Ok && layout.centerOffsetX == 0.0f,
          "Uniform ignores the offset");
}

void TestWorkShift()
{
    // Shifting the raw Work rectangle keeps the band and the Work size; the periphery budget moves
    // between the two sides.
    auto config = ofps::sdk::DefaultConfigV2(); // 80/90 at 4K: peripheries 384 px each, budget 384 px
    float lo = 0.0f, hi = 0.0f;
    ofps::sdk::WorkShiftLimitsPercentV2(config, 0, &lo, &hi);
    Check(Near(hi, 5.0f, 1.0e-3f) && Near(lo, -5.0f, 1.0e-3f),
          "80/90 allows the contour to slide by 5 percent either way");
    ofps::sdk::LayoutV2 base{};
    Check(ofps::sdk::BuildLayout(config, 3840, 2160, &base) == ofps::sdk::Status::Ok, "base layout builds");
    config.workShiftXPercent = 3.0f; // 115.2 px to the right
    ofps::sdk::LayoutV2 shifted{};
    Check(ofps::sdk::ValidateConfig(config) == ofps::sdk::Status::Ok && ofps::sdk::BuildLayout(config, 3840, 2160, &shifted) == ofps::sdk::Status::Ok &&
              ofps::sdk::ValidateLayout(shifted) == ofps::sdk::Status::Ok,
          "shifted layout builds and validates");
    Check(shifted.workWidth == base.workWidth && shifted.workHeight == base.workHeight,
          "the Work shift keeps the Work extent");
    // Left periphery gives 115.2 px: 192 - 115.2 = 76.8 -> 0.2; right takes them: 307.2 -> 0.8.
    Check(Near(shifted.compressionXNeg, 0.2f, 1.0e-3f) && Near(shifted.compressionXPos, 0.8f, 1.0e-3f),
          "the contour slides right: the left side is compressed harder, the right side less");
    Check(Near(shifted.compressionYNeg, 0.5f) && Near(shifted.compressionYPos, 0.5f), "the other axis is untouched");
    // The two peripheries still use the whole budget (76.8 + 307.2 = 384 px).
    Check(Near(shifted.compressionXNeg * 384.0f + shifted.compressionXPos * 384.0f, 384.0f, 0.05f),
          "the shifted split still spends exactly the periphery budget");
    // The band stays where it was and stays 1:1.
    const ofps::sdk::Float2 a = ofps::sdk::PackPosition({1820.5f, 1080.5f}, shifted);
    const ofps::sdk::Float2 b = ofps::sdk::PackPosition({2020.5f, 1080.5f}, shifted);
    Check(Near(b.x - a.x, 200.0f, 1.0e-2f) && Near(a.x - std::floor(a.x), 0.5f, 1.0e-3f),
          "the band is still 1:1 and translated by whole texels");
    float maxError = 0.0f;
    for (int x = 3; x < 3840; x += 97) {
        const ofps::sdk::Float2 native{static_cast<float>(x) + 0.5f, 700.5f};
        const ofps::sdk::Float2 restored = ofps::sdk::UnpackPosition(ofps::sdk::PackPosition(native, shifted), shifted);
        maxError = std::max(maxError, std::abs(restored.x - native.x));
    }
    Check(maxError < 0.01f, "shifted Pack/Unpack round-trip stays subpixel");
    config.workShiftXPercent = 5.2f;
    Check(ofps::sdk::ValidateConfig(config) == ofps::sdk::Status::InvalidAxis, "a shift beyond the limit is rejected");
    config.workShiftXPercent = 3.0f;
    ofps::sdk::ConfigV1 legacy{};
    Check(ofps::sdk::DowngradeConfig(config, &legacy) == ofps::sdk::Status::InvalidAxis, "a shifted configuration does not downgrade");
    ofps::sdk::LayoutV1 legacyLayout{};
    Check(ofps::sdk::DowngradeLayout(shifted, &legacyLayout) == ofps::sdk::Status::InvalidAxis, "a shifted layout does not downgrade");
    // A forged asymmetric split that does not add up is refused.
    ofps::sdk::LayoutV2 forged = shifted;
    forged.compressionXPos = 0.9f;
    Check(ofps::sdk::ValidateLayout(forged) == ofps::sdk::Status::InvalidAxis, "a split that exceeds the budget is rejected");
}

void TestLegacyParityAtGlobal100()
{
    const ofps::sdk::ConfigV1 legacyConfig = ofps::sdk::DefaultConfigV1();
    const ofps::sdk::ConfigV2 modernConfig = ofps::sdk::DefaultConfigV2();
    ofps::sdk::LayoutV1 legacy{};
    ofps::sdk::LayoutV2 modern{};
    Check(ofps::sdk::BuildLayout(legacyConfig, 3840, 2160, &legacy) == ofps::sdk::Status::Ok &&
              ofps::sdk::BuildLayout(modernConfig, 3840, 2160, &modern) == ofps::sdk::Status::Ok,
          "legacy and v2 parity layouts build");
    Check(legacy.workWidth == modern.workWidth && legacy.workHeight == modern.workHeight &&
              Near(legacy.workFractionX, modern.rawWorkFractionX) &&
              Near(legacy.workFractionY, modern.rawWorkFractionY),
          "Global 100 preserves legacy extents and Warp fractions");
    float maxPositionDelta = 0.0f;
    float maxMotionDelta = 0.0f;
    for (int y = 25; y < 2160; y += 149) {
        for (int x = 25; x < 3840; x += 193) {
            const ofps::sdk::Float2 position{static_cast<float>(x) + 0.5f,
                                      static_cast<float>(y) + 0.5f};
            const ofps::sdk::Float2 motion{31.25f, -14.75f};
            const ofps::sdk::Float2 legacyPosition = ofps::sdk::PackPosition(position, legacy);
            const ofps::sdk::Float2 modernPosition = ofps::sdk::PackPosition(position, modern);
            const ofps::sdk::Float2 legacyMotion = ofps::sdk::PackMotion(position, motion, legacy);
            const ofps::sdk::Float2 modernMotion = ofps::sdk::PackMotion(position, motion, modern);
            maxPositionDelta = std::max(
                maxPositionDelta,
                std::max(std::abs(legacyPosition.x - modernPosition.x),
                         std::abs(legacyPosition.y - modernPosition.y)));
            maxMotionDelta = std::max(
                maxMotionDelta,
                std::max(std::abs(legacyMotion.x - modernMotion.x),
                         std::abs(legacyMotion.y - modernMotion.y)));
        }
    }
    Check(maxPositionDelta < 0.001f,
          "Global 100 v2 positions match the legacy path");
    Check(maxMotionDelta < 0.001f,
          "Global 100 v2 motion matches the legacy path well below 0.05 px");
}

} // namespace

int main()
{
    TestAbi();
    TestDefaultAndGlobalScale();
    TestAggressiveAndAsymmetricLayouts();
    TestValidation();
    TestMathRoundTrip();
    TestCenterOffset();
    TestWorkShift();
    TestLegacyParityAtGlobal100();
    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "Optimizer FPS SDK layout-v2 tests passed\n";
    return EXIT_SUCCESS;
}
