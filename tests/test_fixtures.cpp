#include "peripheral_warp/math.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace {

int failures = 0;

void Check(bool condition, const char *message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

float Checker(int x, int y) { return ((x / 4 + y / 4) & 1) != 0 ? 1.0f : 0.0f; }

void TestIdentityFixture()
{
    pw::ConfigV1 config = pw::DefaultConfigV1();
    config.mode = pw::WarpMode::Off;
    pw::LayoutV1 layout{};
    Check(pw::BuildLayout(config, 320, 180, &layout) == pw::Status::Ok, "identity fixture layout builds");
    for (int y = 0; y < 180; ++y) {
        for (int x = 0; x < 320; ++x) {
            const pw::Float2 source = pw::UnpackPosition(pw::PackPosition({x + 0.5f, y + 0.5f}, layout), layout);
            Check(std::abs(source.x - (x + 0.5f)) < 1.0e-6f && std::abs(source.y - (y + 0.5f)) < 1.0e-6f,
                  "identity fixture preserves every pixel center");
        }
    }
}

void TestGridAndCheckerFixture()
{
    pw::LayoutV1 layout{};
    Check(pw::BuildLayout(pw::DefaultConfigV1(), 320, 180, &layout) == pw::Status::Ok,
          "grid fixture layout builds");
    // Every reconstructed native sample maps back to its original continuous coordinate.
    for (int y = 0; y < 180; y += 3) {
        for (int x = 0; x < 320; x += 3) {
            const pw::Float2 native{x + 0.5f, y + 0.5f};
            const pw::Float2 packed = pw::PackPosition(native, layout);
            const pw::Float2 restored = pw::UnpackPosition(packed, layout);
            Check(std::abs(restored.x - native.x) < 0.01f && std::abs(restored.y - native.y) < 0.01f,
                  "grid fixture has no geometric seam");
            if (x >= 32 && x < 288 && y >= 18 && y < 162) {
                const int restoredX = static_cast<int>(restored.x);
                const int restoredY = static_cast<int>(restored.y);
                Check(Checker(x, y) == Checker(restoredX, restoredY), "central checker cells survive the reference round-trip");
            }
        }
    }
}

void TestDepthEdgeFixture()
{
    constexpr int width = 320;
    constexpr int height = 180;
    pw::LayoutV1 layout{};
    Check(pw::BuildLayout(pw::DefaultConfigV1(), width, height, &layout) == pw::Status::Ok,
          "depth fixture layout builds");
    std::vector<float> source(static_cast<std::size_t>(width) * height);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            source[static_cast<std::size_t>(y) * width + x] = x < width / 2 ? 0.1f : 0.9f;
    for (std::uint32_t y = 0; y < layout.workHeight; ++y) {
        for (std::uint32_t x = 0; x < layout.workWidth; ++x) {
            const pw::Float2 native = pw::UnpackPosition({x + 0.5f, y + 0.5f}, layout);
            const int sx = std::clamp(static_cast<int>(native.x), 0, width - 1);
            const int sy = std::clamp(static_cast<int>(native.y), 0, height - 1);
            const float pointDepth = source[static_cast<std::size_t>(sy) * width + sx];
            Check(pointDepth == 0.1f || pointDepth == 0.9f, "point depth fixture never invents an intermediate surface");
        }
    }
}

void TestMotionBoundaryFixture()
{
    pw::LayoutV1 layout{};
    Check(pw::BuildLayout(pw::DefaultConfigV1(), 320, 180, &layout) == pw::Status::Ok,
          "motion fixture layout builds");
    // The central boundary is x=32/288. These vectors cross both boundaries and the frame edge.
    const pw::Float2 positions[] = {{34.5f, 90.5f}, {285.5f, 90.5f}, {5.5f, 5.5f}};
    const pw::Float2 motions[] = {{-12.0f, 0.0f}, {22.0f, -3.0f}, {-40.0f, -20.0f}};
    for (std::size_t i = 0; i < 3; ++i) {
        const pw::Float2 packedPosition = pw::PackPosition(positions[i], layout);
        const pw::Float2 packedMotion = pw::PackMotion(positions[i], motions[i], layout);
        const pw::Float2 restored = pw::UnpackMotion(packedPosition, packedMotion, layout);
        Check(std::abs(restored.x - motions[i].x) < 0.01f && std::abs(restored.y - motions[i].y) < 0.01f,
              "motion crossing a density boundary uses endpoint conversion");
    }
}

void TestPointLoadedMotionGuideCenterFixture()
{
    pw::LayoutV1 layout{};
    Check(pw::BuildLayout(pw::DefaultConfigV1(), 320, 180, &layout) == pw::Status::Ok,
          "point-loaded motion fixture layout builds");

    // A point-loaded vector belongs to the center of the selected guide texel, not
    // the continuously mapped native sample that happened to select that texel.
    const pw::Float2 nativeSample{19.5f, 47.5f};
    const pw::Float2 mappedWork = pw::PackPosition(nativeSample, layout);
    const pw::Float2 guideCenter{std::floor(mappedWork.x) + 0.5f,
                                 std::floor(mappedWork.y) + 0.5f};
    const pw::Float2 nativeGuide = pw::UnpackPosition(guideCenter, layout);
    const pw::Float2 expectedMotion{-23.75f, 11.25f};
    const pw::Float2 packedMotion = pw::PackMotion(nativeGuide, expectedMotion, layout);
    const pw::Float2 decoded = pw::UnpackMotion(guideCenter, packedMotion, layout);
    Check(std::abs(decoded.x - expectedMotion.x) < 0.01f &&
              std::abs(decoded.y - expectedMotion.y) < 0.01f,
          "point-loaded motion decodes at guidePixel + 0.5");

    const pw::Float2 decodedAtWrongPoint = pw::UnpackMotion(mappedWork, packedMotion, layout);
    Check(std::abs(decodedAtWrongPoint.x - expectedMotion.x) > 0.01f ||
              std::abs(decodedAtWrongPoint.y - expectedMotion.y) > 0.01f,
          "fixture distinguishes guide-center decoding from continuous-position decoding");
}

} // namespace

int main()
{
    TestIdentityFixture();
    TestGridAndCheckerFixture();
    TestDepthEdgeFixture();
    TestMotionBoundaryFixture();
    TestPointLoadedMotionGuideCenterFixture();
    if (failures != 0) {
        std::cerr << failures << " fixture test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "PeripheralWarp shader-reference fixtures passed\n";
    return EXIT_SUCCESS;
}
