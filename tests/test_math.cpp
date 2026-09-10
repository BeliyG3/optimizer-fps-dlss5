#include "peripheral_warp/math.h"

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

bool Near(float a, float b, float tolerance = 1.0e-4f)
{
    return std::abs(a - b) <= tolerance;
}

void TestDefaultLayout()
{
    const pw::ConfigV1 config = pw::DefaultConfigV1();
    pw::LayoutV1 layout{};
    Check(pw::BuildLayout(config, 3840, 2160, &layout) == pw::Status::Ok, "default layout builds");
    Check(layout.workWidth == 3456 && layout.workHeight == 1944, "80->90 uses 3456x1944 at 4K");
    Check(Near(layout.centerFractionX, 0.8f) && Near(layout.workFractionX, 0.9f), "X fractions are exact");
    Check(Near(layout.compressionX, 0.5f) && Near(layout.edgeSlopeX, 0.25f), "default edge slope is one quarter");
}

void TestRoundTrip()
{
    pw::LayoutV1 layout{};
    Check(pw::BuildLayout(pw::DefaultConfigV1(), 3840, 2160, &layout) == pw::Status::Ok, "round-trip layout builds");
    float maxError = 0.0f;
    for (int y = -128; y <= 2288; y += 17) {
        for (int x = -128; x <= 3968; x += 19) {
            const pw::Float2 p{static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f};
            const pw::Float2 q = pw::PackPosition(p, layout);
            const pw::Float2 r = pw::UnpackPosition(q, layout);
            maxError = std::max(maxError, std::max(std::abs(r.x - p.x), std::abs(r.y - p.y)));
        }
    }
    Check(maxError < 0.01f, "pack/unpack stays below 0.01 pixel error including edge extension");
}

void TestCurveProperties()
{
    constexpr float center = 0.8f;
    constexpr float work = 0.9f;
    constexpr float h = 1.0e-3f;
    const float compression = (work - center) / (1.0f - center);
    const float expectedEdgeSlope = compression * compression;
    const float derivativeAtCenter =
        (pw::PackRadius(center + h, center, work) - pw::PackRadius(center - h, center, work)) / (2.0f * h);
    const float derivativeBeforeEdge =
        (pw::PackRadius(1.0f, center, work) - pw::PackRadius(1.0f - h, center, work)) / h;
    const float derivativeAfterEdge =
        (pw::PackRadius(1.0f + h, center, work) - pw::PackRadius(1.0f, center, work)) / h;
    Check(Near(derivativeAtCenter, 1.0f, 0.003f), "curve is C1 at the center boundary");
    Check(Near(derivativeBeforeEdge, expectedEdgeSlope, 0.003f), "curve reaches the analytic edge slope");
    Check(Near(derivativeAfterEdge, expectedEdgeSlope, 0.003f), "edge extension preserves the edge derivative");

    pw::LayoutV1 layout{};
    Check(pw::BuildLayout(pw::DefaultConfigV1(), 3840, 2160, &layout) == pw::Status::Ok,
          "curve-property layout builds");
    for (std::uint32_t axis = 0; axis != 2; ++axis) {
        const float nativeExtent = axis == 0 ? static_cast<float>(layout.nativeWidth) : static_cast<float>(layout.nativeHeight);
        const float workExtent = axis == 0 ? static_cast<float>(layout.workWidth) : static_cast<float>(layout.workHeight);
        float previous = pw::PackCoordinate(-64.0f, axis, layout);
        for (float value = -63.75f; value <= nativeExtent + 64.0f; value += 0.25f) {
            const float current = pw::PackCoordinate(value, axis, layout);
            Check(current > previous, "packed coordinate is strictly monotonic");
            previous = current;
        }
        for (float value = -32.0f; value <= nativeExtent + 32.0f; value += 7.25f) {
            const float left = pw::PackCoordinate(value, axis, layout);
            const float right = pw::PackCoordinate(nativeExtent - value, axis, layout);
            Check(Near(left + right, workExtent, 0.002f), "packed coordinate is center-symmetric");
        }
    }
}

void TestMotionRoundTrip()
{
    pw::LayoutV1 layout{};
    Check(pw::BuildLayout(pw::DefaultConfigV1(), 3840, 2160, &layout) == pw::Status::Ok, "motion layout builds");
    const pw::Float2 positions[] = {{1920.5f, 1080.5f}, {200.5f, 100.5f}, {3500.5f, 2000.5f}};
    const pw::Float2 motions[] = {{12.25f, -7.5f}, {-80.0f, 24.0f}, {500.0f, -300.0f}};
    for (const auto &position : positions) {
        const pw::Float2 packedPosition = pw::PackPosition(position, layout);
        for (const auto &motion : motions) {
            const pw::Float2 packedMotion = pw::PackMotion(position, motion, layout);
            const pw::Float2 restored = pw::UnpackMotion(packedPosition, packedMotion, layout);
            Check(Near(restored.x, motion.x, 0.01f) && Near(restored.y, motion.y, 0.01f),
                  "motion vectors round-trip through endpoints");
        }
    }
}

void TestIdentityAndUniform()
{
    pw::ConfigV1 config = pw::DefaultConfigV1();
    config.mode = pw::WarpMode::Off;
    pw::LayoutV1 off{};
    Check(pw::BuildLayout(config, 3840, 2160, &off) == pw::Status::Ok, "off layout builds");
    Check(off.workWidth == 3840 && Near(pw::PackCoordinate(101.25f, 0, off), 101.25f), "off is identity");

    config.mode = pw::WarpMode::Uniform;
    config.xAxis.workPercent = 90.0f;
    config.yAxis.workPercent = 90.0f;
    pw::LayoutV1 uniform{};
    Check(pw::BuildLayout(config, 3840, 2160, &uniform) == pw::Status::Ok, "uniform layout builds");
    Check(Near(pw::PackCoordinate(1920.0f, 0, uniform), 1728.0f), "uniform scales coordinates");
}

void TestValidation()
{
    pw::ConfigV1 config = pw::DefaultConfigV1();
    config.xAxis.workPercent = 85.0f;
    Check(pw::ValidateConfig(config) == pw::Status::InvalidAxis, "compression guard rejects an edge slope below one quarter");
    config = pw::DefaultConfigV1();
    config.xAxis.centerPercent = 79.99999f;
    config.xAxis.workPercent = 89.99999f;
    Check(pw::ValidateConfig(config) == pw::Status::Ok,
          "float round-trip at the exact compression boundary is accepted");
    config.xAxis.centerPercent = 0.0f;
    Check(pw::ValidateConfig(config) == pw::Status::InvalidAxis,
          "zero center remains invalid");
    config.xAxis.centerPercent = -1.0f;
    Check(pw::ValidateConfig(config) == pw::Status::InvalidAxis,
          "negative center remains invalid");

    config = pw::DefaultConfigV1();
    config.structSize = 0;
    Check(pw::ValidateConfig(config) == pw::Status::StructSizeMismatch, "structure size is enforced");
    config = pw::DefaultConfigV1();
    pw::LayoutV1 layout{};
    Check(pw::BuildLayout(config, 3839, 2160, &layout) == pw::Status::Ok && (layout.workWidth & 1u) == 0,
          "peripheral work extent remains even for odd native sizes");

    config.mode = pw::WarpMode::Uniform;
    config.xAxis.workPercent = 91.0f;
    config.yAxis.workPercent = 91.0f;
    Check(pw::BuildLayout(config, 3840, 2160, &layout) == pw::Status::Ok &&
          layout.workWidth == 3496 && layout.workHeight == 1966,
          "uniform mode uses canonical ceil-to-even extents");

    Check(pw::ValidateLayout(layout) == pw::Status::Ok, "a canonical layout validates");
    layout.edgeSlopeX = 0.75f;
    Check(pw::ValidateLayout(layout) == pw::Status::InvalidAxis,
          "layout validation rejects inconsistent derived values");

    config = pw::DefaultConfigV1();
    config.reserved[0] = 1;
    Check(pw::ValidateConfig(config) == pw::Status::InvalidFlags,
          "configuration validation rejects nonzero reserved fields");
}

void TestMotionEdgePolicy()
{
    pw::ConfigV1 config = pw::DefaultConfigV1();
    config.flags &= ~pw::ConfigFlagExtendMotionAtEdge;
    pw::LayoutV1 clamped{};
    Check(pw::BuildLayout(config, 320, 180, &clamped) == pw::Status::Ok,
          "clamped-motion layout builds");

    const pw::Float2 current{0.5f, 90.5f};
    const pw::Float2 outsideMotion{-40.0f, 0.0f};
    const pw::Float2 clampedMotion = pw::PackMotion(current, outsideMotion, clamped);
    Check(Near(clampedMotion.x, 0.0f), "cleared edge flag clamps motion endpoints to the first pixel center");

    config.flags |= pw::ConfigFlagExtendMotionAtEdge;
    pw::LayoutV1 extended{};
    Check(pw::BuildLayout(config, 320, 180, &extended) == pw::Status::Ok,
          "extended-motion layout builds");
    const pw::Float2 extendedMotion = pw::PackMotion(current, outsideMotion, extended);
    Check(extendedMotion.x < -0.1f, "set edge flag linearly continues motion beyond the frame");
}

} // namespace

int main()
{
    TestDefaultLayout();
    TestRoundTrip();
    TestCurveProperties();
    TestMotionRoundTrip();
    TestIdentityAndUniform();
    TestValidation();
    TestMotionEdgePolicy();
    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "PeripheralWarp CPU tests passed\n";
    return EXIT_SUCCESS;
}
