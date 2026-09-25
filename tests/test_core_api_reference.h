#pragma once
#include "optimizer_fps/math.h"
#include "test_core_api_pattern.h"
namespace coretest {
inline Pixel Sample(const Image &a, std::uint32_t w, std::uint32_t h, ofps::sdk::Float2 pos) {
    const float px = std::clamp(pos.x - 0.5f, 0.0f, static_cast<float>(w - 1));
    const float py = std::clamp(pos.y - 0.5f, 0.0f, static_cast<float>(h - 1));
    const auto x = static_cast<std::uint32_t>(px), y = static_cast<std::uint32_t>(py);
    const auto xx = std::min(x + 1, w - 1), yy = std::min(y + 1, h - 1);
    Pixel p{};
    for (std::size_t c = 0; c < 4; ++c)
        p[c] = std::lerp(std::lerp(a[y * w + x][c], a[y * w + xx][c], px - x),
                         std::lerp(a[yy * w + x][c], a[yy * w + xx][c], px - x), py - y);
    return p;
}
// Bilinear CPU texture sampling with SDK coordinate/motion mappings. The scenarios
// explicitly select this filter and the same Peripheral 80/90 layout. Half rounding
// models the R16G16B16A16_FLOAT intermediate and output textures.
struct Reference {
    ofps::sdk::LayoutV2 layout{};
    Image packed, output;
    explicit Reference(std::uint32_t w = kW, std::uint32_t h = kH, float gain = 1.0f) {
        auto config = ofps::sdk::DefaultConfigV2();
        config.colorFilter = ofps::sdk::ColorFilter::Bilinear;
        config.xAxis = config.yAxis = {80.0f, 90.0f};
        if (ofps::sdk::BuildLayout(config, w, h, &layout) != ofps::sdk::Status::Ok)
            return;
        const auto src = Source(w, h, gain);
        packed.resize(static_cast<std::size_t>(layout.workWidth) * layout.workHeight);
        for (std::uint32_t y = 0; y < layout.workHeight; ++y)
            for (std::uint32_t x = 0; x < layout.workWidth; ++x) {
                auto p = Sample(src, w, h, ofps::sdk::UnpackPosition({x + 0.5f, y + 0.5f}, layout));
                for (auto &v : p)
                    v = Half(v);
                packed[y * layout.workWidth + x] = p;
            }
        output.resize(static_cast<std::size_t>(w) * h);
        for (std::uint32_t y = 0; y < h; ++y)
            for (std::uint32_t x = 0; x < w; ++x) {
                auto p = Sample(packed, layout.workWidth, layout.workHeight,
                                ofps::sdk::PackPosition({x + 0.5f, y + 0.5f}, layout));
                for (auto &v : p)
                    v = Half(v);
                output[y * w + x] = p;
            }
    }
};
inline bool MatchesImage(const ReadbackCapture &capture, const Image &expected, std::uint32_t w, std::uint32_t h,
                         float eps = kSdkTolerance, std::uint32_t ox = 0, std::uint32_t oy = 0) {
    if (!capture.buffer || expected.size() != static_cast<std::size_t>(w) * h ||
        ox + w > capture.footprint.Footprint.Width || oy + h > capture.footprint.Footprint.Height)
        return false;
    void *mapped = nullptr;
    const D3D12_RANGE range{0, static_cast<SIZE_T>(capture.byteCount)};
    if (FAILED(capture.buffer->Map(0, &range, &mapped)))
        return false;
    bool ok = true;
    for (std::uint32_t y = 0; y < h && ok; ++y)
        for (std::uint32_t x = 0; x < w && ok; ++x) {
            const auto *row = static_cast<const std::byte *>(mapped) + capture.footprint.Offset +
                              static_cast<std::size_t>(y + oy) * capture.footprint.Footprint.RowPitch;
            std::uint16_t p[4];
            std::memcpy(p, row + (x + ox) * sizeof(p), sizeof(p));
            for (std::size_t c = 0; c < 4; ++c)
                if (!Near(HalfToFloat(p[c]), expected[y * w + x][c], eps)) {
                    std::cerr << "pixel " << x << ',' << y << " channel " << c << ": " << HalfToFloat(p[c])
                              << " expected " << expected[y * w + x][c] << '\n';
                    ok = false;
                    break;
                }
        }
    const D3D12_RANGE noWrite{0, 0};
    capture.buffer->Unmap(0, &noWrite);
    return ok;
}
inline bool MatchesPackedGuides(const ReadbackCapture &depth, const ReadbackCapture &motion) {
    const Reference ref;
    if (depth.footprint.Footprint.Width != kWorkW || depth.footprint.Footprint.Height != kWorkH ||
        motion.footprint.Footprint.Width != kWorkW || motion.footprint.Footprint.Height != kWorkH)
        return false;
    for (std::uint32_t y = 0; y < kWorkH; ++y)
        for (std::uint32_t x = 0; x < kWorkW; ++x) {
            const auto n = ofps::sdk::UnpackPosition({x + 0.5f, y + 0.5f}, ref.layout);
            const auto nx = static_cast<std::uint32_t>(std::clamp(n.x, 0.0f, static_cast<float>(kW - 1)));
            const auto ny = static_cast<std::uint32_t>(std::clamp(n.y, 0.0f, static_cast<float>(kH - 1)));
            const auto mv = Motion(nx, ny);
            const auto expected = ofps::sdk::PackMotion(n, {mv.x * 2.0f, mv.y * 3.0f}, ref.layout);
            float d = 0;
            std::uint16_t m[2]{};
            if (!ReadPixelBytes(depth, x, y, &d, sizeof(d)) || !ReadPixelBytes(motion, x, y, m, sizeof(m)) ||
                !Near(d, Depth(nx, ny), 1.0e-6f) || !Near(HalfToFloat(m[0]), expected.x, 0.03f) ||
                !Near(HalfToFloat(m[1]), expected.y, 0.03f)) {
                std::cerr << "packed guide mismatch at " << x << ',' << y << '\n';
                return false;
            }
        }
    return true;
}
inline bool MatchesSdkReference(const ReadbackCapture &c) {
    static const Reference r;
    return MatchesImage(c, r.output, kW, kH);
}
inline bool MatchesSource(const ReadbackCapture &c, std::uint32_t ox = 0, std::uint32_t oy = 0) {
    static const auto src = Source();
    return MatchesImage(c, src, kW, kH, 1.0e-6f, ox, oy);
}
} // namespace coretest
