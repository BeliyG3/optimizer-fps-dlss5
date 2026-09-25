#pragma once
#include "optimizer_fps/types.h"
#include "test_core_api_gpu.h"
#include <DirectXPackedVector.h>
#include <algorithm>
namespace coretest {
constexpr std::uint32_t kW = 640, kH = 360, kPadW = 1024, kPadH = 512;
constexpr std::uint32_t kWorkW = 576, kWorkH = 324;
constexpr DXGI_FORMAT kColorFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
constexpr D3D12_RESOURCE_STATES kInputRest = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
constexpr D3D12_RESOURCE_STATES kOutputRest = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
constexpr std::array<float, 4> kMarker{0.125f, 0.0625f, 0.875f, 0.5f};
constexpr float kSdkTolerance = 0.02f;
using Pixel = std::array<float, 4>;
using Image = std::vector<Pixel>;
inline bool Near(float a, float b, float eps) { return std::abs(a - b) <= eps; }
inline float Half(float v) { return HalfToFloat(DirectX::PackedVector::XMConvertFloatToHalf(v)); }
// Gradients plus independent fine detail: a direct copy must fail the round-trip oracle.
inline Pixel Pattern(std::uint32_t x, std::uint32_t y, std::uint32_t w = kW, std::uint32_t h = kH) {
    const float u = static_cast<float>(x) / w, v = static_cast<float>(y) / h;
    return {Half(0.1f + 0.45f * u + 0.2f * v + 0.12f * std::sin(1.7f * x)),
            Half(0.15f + 0.2f * u + 0.4f * v + 0.1f * std::cos(1.3f * y)),
            Half(0.3f + 0.15f * u - 0.1f * v + 0.15f * std::sin(0.7f * x + 0.9f * y)),
            Half(0.6f + 0.2f * u + 0.1f * v)};
}
inline float Depth(std::uint32_t x, std::uint32_t y) {
    return 0.1f + 0.4f * static_cast<float>(x) / kW + 0.3f * static_cast<float>(y) / kH;
}
inline ofps::sdk::Float2 Motion(std::uint32_t x, std::uint32_t y) {
    return {Half(1.0f + 3.0f * static_cast<float>(x) / kW), Half(-2.0f + static_cast<float>(y) / kH)};
}
inline Image Source(std::uint32_t w = kW, std::uint32_t h = kH, float gain = 1.0f) {
    Image a(static_cast<std::size_t>(w) * h);
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x) {
            auto p = Pattern(x, y, w, h);
            for (auto &v : p)
                v = Half(v * gain);
            a[y * w + x] = p;
        }
    return a;
}
// Upload resources stay owned by the caller until its submission has completed.
inline bool UploadPattern(WarpDevice &w, ID3D12Resource *target, D3D12_RESOURCE_STATES rest, int kind,
                          std::vector<ComPtr<ID3D12Resource>> &uploads, float gain = 1.0f) {
    auto footprint = CreateReadback(w.device.Get(), target);
    if (!footprint.buffer)
        return false;
    auto desc = footprint.buffer->GetDesc();
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    ComPtr<ID3D12Resource> upload;
    if (FAILED(w.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                 nullptr, IID_PPV_ARGS(&upload))))
        return false;
    void *mapped = nullptr;
    const D3D12_RANGE noRead{0, 0};
    if (FAILED(upload->Map(0, &noRead, &mapped)))
        return false;
    const auto tw = footprint.footprint.Footprint.Width, th = footprint.footprint.Footprint.Height;
    for (std::uint32_t y = 0; y < th; ++y)
        for (std::uint32_t x = 0; x < tw; ++x) {
            auto *row = static_cast<std::byte *>(mapped) + footprint.footprint.Offset +
                        static_cast<std::size_t>(y) * footprint.footprint.Footprint.RowPitch;
            if (kind == 1) {
                const float d = Depth(x, y);
                std::memcpy(row + x * 4, &d, 4);
            } else {
                const auto m = Motion(x, y);
                auto p = kind == 0 ? Pattern(x, y, tw, th) : Pixel{m.x, m.y, 0, 0};
                std::uint16_t raw[4]{};
                const std::size_t channels = kind == 0 ? 4 : 2;
                for (std::size_t c = 0; c < channels; ++c)
                    raw[c] = DirectX::PackedVector::XMConvertFloatToHalf(p[c] * gain);
                std::memcpy(row + x * channels * 2, raw, channels * 2);
            }
        }
    upload->Unmap(0, nullptr);
    D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
    src.pResource = upload.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint = footprint.footprint;
    dst.pResource = target;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    Transition(w.list.Get(), target, rest, D3D12_RESOURCE_STATE_COPY_DEST);
    w.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    Transition(w.list.Get(), target, D3D12_RESOURCE_STATE_COPY_DEST, rest);
    uploads.push_back(upload);
    return true;
}
} // namespace coretest
