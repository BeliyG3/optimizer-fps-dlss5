#include "test_core_api_half_ulp.h"

#include <array>
#include <cmath>
#include <cstring>
#include <iostream>

namespace coretest {
bool PackedMotionWithinWorkPixelThreshold(const ReadbackCapture &pixel, const ReadbackCapture &compute) {
    if (!pixel.buffer || !compute.buffer ||
        pixel.footprint.Footprint.Width != compute.footprint.Footprint.Width ||
        pixel.footprint.Footprint.Height != compute.footprint.Footprint.Height ||
        pixel.footprint.Footprint.Format != DXGI_FORMAT_R16G16_FLOAT ||
        compute.footprint.Footprint.Format != DXGI_FORMAT_R16G16_FLOAT) return false;
    void *p = nullptr, *c = nullptr;
    const D3D12_RANGE pr{0, static_cast<SIZE_T>(pixel.byteCount)};
    const D3D12_RANGE cr{0, static_cast<SIZE_T>(compute.byteCount)};
    if (FAILED(pixel.buffer->Map(0, &pr, &p))) return false;
    if (FAILED(compute.buffer->Map(0, &cr, &c))) {
        pixel.buffer->Unmap(0, nullptr);
        return false;
    }
    constexpr float limit = 1.0f / 64.0f;
    std::array<float, 2> maximum{};
    bool finite = true;
    const auto width = pixel.footprint.Footprint.Width;
    const auto height = pixel.footprint.Footprint.Height;
    for (std::uint32_t y = 0; y < height; ++y) {
        const auto *a = static_cast<const std::byte *>(p) + pixel.footprint.Offset +
                        static_cast<std::size_t>(y) * pixel.footprint.Footprint.RowPitch;
        const auto *b = static_cast<const std::byte *>(c) + compute.footprint.Offset +
                        static_cast<std::size_t>(y) * compute.footprint.Footprint.RowPitch;
        for (std::uint32_t x = 0; x < width; ++x)
            for (std::uint32_t channel = 0; channel < 2; ++channel) {
                std::uint16_t left, right;
                std::memcpy(&left, a + x * 4 + channel * 2, sizeof(left));
                std::memcpy(&right, b + x * 4 + channel * 2, sizeof(right));
                const float distance = std::abs(HalfToFloat(left) - HalfToFloat(right));
                if (!std::isfinite(distance)) finite = false;
                else if (distance > maximum[channel]) maximum[channel] = distance;
            }
    }
    const D3D12_RANGE noWrite{0, 0};
    compute.buffer->Unmap(0, &noWrite);
    pixel.buffer->Unmap(0, &noWrite);
    std::cerr << "packed motion maximum absolute work-pixel difference X=" << maximum[0]
              << " Y=" << maximum[1] << " limit=" << limit << '\n';
    return finite && maximum[0] <= limit && maximum[1] <= limit;
}
} // namespace coretest
