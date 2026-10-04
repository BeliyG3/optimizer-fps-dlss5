#pragma once
// Helpers of the TemporalGrid scenario (test_core_api_grid.cpp): a fake model whose edit is a fixed one-texel
// checker, so the residual it leaves has detail at every model texel and a carried frame reprojected on a coarser
// grid comes out different from the native route; and float images read back from a capture, to compare frames.
#include "test_core_api_fakes.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <vector>

namespace coretest {

struct CheckerModelHost : FakeModelHost {
    ComPtr<ID3D12Resource> edit; // kW x kH, rests in COMMON: promoted to COPY_SOURCE on whichever queue runs the model

    bool Initialize(WarpDevice &w) {
        edit = CreateTexture(w.device.Get(), kW, kH, kColorFormat, D3D12_RESOURCE_FLAG_NONE,
                             D3D12_RESOURCE_STATE_COPY_DEST);
        auto footprint = CreateReadback(w.device.Get(), edit.Get());
        if (!edit || !footprint.buffer || !BeginList(w))
            return false;
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        const D3D12_RESOURCE_DESC desc = footprint.buffer->GetDesc();
        ComPtr<ID3D12Resource> upload;
        void *mapped = nullptr;
        const D3D12_RANGE noRead{0, 0};
        if (FAILED(w.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                     D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload))) ||
            FAILED(upload->Map(0, &noRead, &mapped)))
            return false;
        for (std::uint32_t y = 0; y < kH; ++y)
            for (std::uint32_t x = 0; x < kW; ++x) {
                const float v = ((x + y) & 1u) != 0 ? 0.85f : 0.15f;
                const std::uint16_t texel[4] = {DirectX::PackedVector::XMConvertFloatToHalf(v),
                                                DirectX::PackedVector::XMConvertFloatToHalf(1.0f - v),
                                                DirectX::PackedVector::XMConvertFloatToHalf(v),
                                                DirectX::PackedVector::XMConvertFloatToHalf(1.0f)};
                std::memcpy(static_cast<std::byte *>(mapped) + footprint.footprint.Offset +
                                static_cast<std::size_t>(y) * footprint.footprint.Footprint.RowPitch + x * sizeof(texel),
                            texel, sizeof(texel));
            }
        upload->Unmap(0, nullptr);
        D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
        src.pResource = upload.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint = footprint.footprint;
        dst.pResource = edit.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        w.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        Transition(w.list.Get(), edit.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
        return SubmitList(w) && WaitForQueue(w.device.Get(), w.queue.Get());
    }

    // The copy model's bookkeeping, then the checker over its output rectangle.
    int RunModel(ID3D12GraphicsCommandList *cmd, void *handle, const OfpsModelInputs *inputs) override {
        const int result = FakeModelHost::RunModel(cmd, handle, inputs);
        if (result != OFPS_OK || !edit)
            return result;
        const OfpsResource &out = inputs->output;
        Transition(cmd, out.res, out.restState, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
        src.pResource = edit.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.pResource = out.res;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        const D3D12_BOX box{0, 0, 0, std::min(out.rect.w, kW), std::min(out.rect.h, kH), 1};
        cmd->CopyTextureRegion(&dst, out.rect.x, out.rect.y, 0, &src, &box);
        Transition(cmd, out.res, D3D12_RESOURCE_STATE_COPY_DEST, out.restState);
        return OFPS_OK;
    }
};

// RGB of a kW x kH half-float capture.
inline std::vector<float> ReadRgb(const ReadbackCapture &capture) {
    std::vector<float> rgb;
    void *mapped = nullptr;
    const D3D12_RANGE range{0, static_cast<SIZE_T>(capture.byteCount)};
    if (!capture.buffer || capture.footprint.Footprint.Width < kW || capture.footprint.Footprint.Height < kH ||
        FAILED(capture.buffer->Map(0, &range, &mapped)))
        return rgb;
    rgb.reserve(static_cast<std::size_t>(kW) * kH * 3);
    for (std::uint32_t y = 0; y < kH; ++y)
        for (std::uint32_t x = 0; x < kW; ++x) {
            std::uint16_t texel[4];
            std::memcpy(texel, static_cast<const std::byte *>(mapped) + capture.footprint.Offset +
                                   static_cast<std::size_t>(y) * capture.footprint.Footprint.RowPitch + x * sizeof(texel),
                        sizeof(texel));
            for (int c = 0; c < 3; ++c) rgb.push_back(HalfToFloat(texel[c]));
        }
    const D3D12_RANGE noWrite{0, 0};
    capture.buffer->Unmap(0, &noWrite);
    return rgb;
}

inline std::vector<float> SourceRgb() {
    std::vector<float> rgb;
    for (const Pixel &p : Source()) rgb.insert(rgb.end(), p.begin(), p.begin() + 3);
    return rgb;
}

struct Difference {
    bool valid = false;            // same size, every value finite
    std::size_t pixels = 0;        // pixels with any channel different
    float max = 0.0f, mean = 0.0f; // over channels
};
inline Difference Compare(const std::vector<float> &a, const std::vector<float> &b) {
    Difference d;
    if (a.empty() || a.size() != b.size()) return d;
    d.valid = true;
    double sum = 0.0;
    for (std::size_t i = 0; i < a.size(); i += 3) {
        bool differs = false;
        for (std::size_t c = i; c < i + 3; ++c) {
            if (!std::isfinite(a[c]) || !std::isfinite(b[c])) d.valid = false;
            const float e = std::abs(a[c] - b[c]);
            differs = differs || a[c] != b[c];
            d.max = std::max(d.max, e);
            sum += e;
        }
        d.pixels += differs ? 1 : 0;
    }
    d.mean = static_cast<float>(sum / static_cast<double>(a.size()));
    return d;
}

} // namespace coretest
