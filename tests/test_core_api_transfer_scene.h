#pragma once
// Detail-transfer test scene: pixel formats, uploads, and one ComputePath with its frame, depth, answer
// and output, packed and unpacked on the WARP device. Used by test_core_api_compute_transfer.cpp.
#include "test_core_api_checks.h"
#include "test_core_api_reference.h"
#include "core/warp/compute.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace coretest::transfer {
using Fill = std::function<float(std::uint32_t, std::uint32_t, int)>;
using PixelFill = std::function<std::array<float, 4>(std::uint32_t, std::uint32_t)>;
using Encode = std::function<void(std::uint32_t, std::uint32_t, std::byte *)>;

// Colour with one-pixel detail that a 50 % stretch cannot keep.
inline float Checker(std::uint32_t x, std::uint32_t y, int ch) {
    if (ch == 3) return 1.0f;
    return ((x + y) & 1u) ? 0.30f : 0.60f;
}
inline std::array<float, 4> CheckerPixel(std::uint32_t x, std::uint32_t y) {
    const float v = Checker(x, y, 0);
    return {v, v, v, 1.0f};
}

inline std::uint32_t Bytes(DXGI_FORMAT f) { return f == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8u : 4u; }

// RGBA16F, R10G10B10A2_UNORM, otherwise four 8-bit UNORM channels (the bytes of an sRGB view too).
inline void EncodePixel(DXGI_FORMAT f, const std::array<float, 4> &c, std::byte *out) {
    if (f == DXGI_FORMAT_R16G16B16A16_FLOAT) {
        std::uint16_t h[4];
        for (int i = 0; i < 4; ++i) h[i] = DirectX::PackedVector::XMConvertFloatToHalf(c[i]);
        std::memcpy(out, h, 8);
    } else if (f == DXGI_FORMAT_R10G10B10A2_UNORM) {
        const auto q = [&](int i, float m) { return static_cast<std::uint32_t>(std::lround(std::clamp(c[i], 0.0f, 1.0f) * m)); };
        const std::uint32_t v = q(0, 1023) | (q(1, 1023) << 10) | (q(2, 1023) << 20) | (q(3, 3) << 30);
        std::memcpy(out, &v, 4);
    } else {
        for (int i = 0; i < 4; ++i) out[i] = static_cast<std::byte>(std::lround(std::clamp(c[i], 0.0f, 1.0f) * 255.0f));
    }
}

inline std::array<float, 4> DecodePixel(DXGI_FORMAT f, const std::byte *in) {
    std::array<float, 4> c{};
    if (f == DXGI_FORMAT_R16G16B16A16_FLOAT) {
        std::uint16_t h[4];
        std::memcpy(h, in, 8);
        for (int i = 0; i < 4; ++i) c[i] = HalfToFloat(h[i]);
    } else if (f == DXGI_FORMAT_R10G10B10A2_UNORM) {
        std::uint32_t v;
        std::memcpy(&v, in, 4);
        c = {(v & 1023u) / 1023.0f, ((v >> 10) & 1023u) / 1023.0f, ((v >> 20) & 1023u) / 1023.0f, (v >> 30) / 3.0f};
    } else {
        for (int i = 0; i < 4; ++i) c[i] = static_cast<float>(std::to_integer<std::uint8_t>(in[i])) / 255.0f;
    }
    return c;
}

inline std::array<float, 4> ReadPixel(const ReadbackCapture &c, DXGI_FORMAT f, std::uint32_t x, std::uint32_t y) {
    std::byte raw[8]{};
    ReadPixelBytes(c, x, y, raw, Bytes(f));
    return DecodePixel(f, raw);
}

// Writes tw x th pixels of bytesPerPixel each through an upload buffer into subresource 0 of target.
inline bool UploadBytes(WarpDevice &w, ID3D12Resource *target, D3D12_RESOURCE_STATES rest, std::uint32_t tw,
                        std::uint32_t th, std::uint32_t bytesPerPixel, const Encode &encode,
                        std::vector<ComPtr<ID3D12Resource>> &uploads) {
    const auto footprint = CreateReadback(w.device.Get(), target);
    if (!footprint.buffer) return false;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    const auto desc = footprint.buffer->GetDesc();
    ComPtr<ID3D12Resource> upload;
    if (FAILED(w.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload)))) return false;
    void *mapped = nullptr;
    const D3D12_RANGE noRead{0, 0};
    if (FAILED(upload->Map(0, &noRead, &mapped))) return false;
    for (std::uint32_t y = 0; y < th; ++y) {
        auto *row = static_cast<std::byte *>(mapped) + footprint.footprint.Offset +
                    static_cast<std::size_t>(y) * footprint.footprint.Footprint.RowPitch;
        for (std::uint32_t x = 0; x < tw; ++x) encode(x, y, row + static_cast<std::size_t>(x) * bytesPerPixel);
    }
    upload->Unmap(0, nullptr);
    D3D12_TEXTURE_COPY_LOCATION from{}, to{};
    from.pResource = upload.Get();
    from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    from.PlacedFootprint = footprint.footprint;
    to.pResource = target;
    to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    Transition(w.list.Get(), target, rest, D3D12_RESOURCE_STATE_COPY_DEST);
    w.list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    Transition(w.list.Get(), target, D3D12_RESOURCE_STATE_COPY_DEST, rest);
    uploads.push_back(upload);
    return true;
}

inline bool UploadPixels(WarpDevice &w, ID3D12Resource *target, D3D12_RESOURCE_STATES rest, DXGI_FORMAT f,
                         std::uint32_t tw, std::uint32_t th, const PixelFill &value,
                         std::vector<ComPtr<ID3D12Resource>> &uploads) {
    return UploadBytes(w, target, rest, tw, th, Bytes(f),
                       [&](std::uint32_t x, std::uint32_t y, std::byte *out) { EncodePixel(f, value(x, y), out); }, uploads);
}

// R32F depth, value of channel 0.
inline bool UploadDepth(WarpDevice &w, ID3D12Resource *target, D3D12_RESOURCE_STATES rest, const Fill &value,
                        std::vector<ComPtr<ID3D12Resource>> &uploads) {
    return UploadBytes(w, target, rest, kW, kH, 4u, [&](std::uint32_t x, std::uint32_t y, std::byte *out) {
        const float d = value(x, y, 0);
        std::memcpy(out, &d, 4);
    }, uploads);
}

struct TransferScene {
    std::string tag;
    ofps::sdk::LayoutV2 layout{};
    std::unique_ptr<ofps::core::warp::ComputePath> path;
    DXGI_FORMAT frameFormat = kColorFormat;  // the host colour texture and its source view
    DXGI_FORMAT packFormat = kColorFormat;   // the model input (ComputePath colour view)
    DXGI_FORMAT answerFormat = kColorFormat; // the model answer and the output
    ComPtr<ID3D12Resource> color, depth, motion, answer, output;
    ofps::sdk::InputDescriptionV2 input{};
    ofps::core::warp::Packed packed{};
    ReadbackCapture packedRead; // the model input as the Pack wrote it
    std::vector<ComPtr<ID3D12Resource>> uploads;
};

inline bool CreateScene(WarpDevice &w, const ofps::sdk::ConfigV2 &config, DXGI_FORMAT frameFormat,
                        DXGI_FORMAT packFormat, DXGI_FORMAT answerFormat, TransferScene &s) {
    s.frameFormat = frameFormat;
    s.packFormat = packFormat;
    s.answerFormat = answerFormat;
    Check(ofps::sdk::BuildLayout(config, kW, kH, &s.layout) == ofps::sdk::Status::Ok, "transfer layout builds: " + s.tag);
    std::string reason;
    s.path = ofps::core::warp::ComputePath::Create(w.device.Get(), s.layout, packFormat, answerFormat, 1, reason);
    Check(s.path != nullptr, "transfer ComputePath creates: " + s.tag);
    if (!s.path) { std::cerr << reason << '\n'; return false; }
    s.color = CreateTexture(w.device.Get(), kW, kH, frameFormat, D3D12_RESOURCE_FLAG_NONE, kInputRest);
    s.depth = CreateTexture(w.device.Get(), kW, kH, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_NONE, kInputRest);
    s.motion = CreateTexture(w.device.Get(), kW, kH, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_NONE, kInputRest);
    s.answer = CreateTexture(w.device.Get(), s.layout.workWidth, s.layout.workHeight, answerFormat,
                             D3D12_RESOURCE_FLAG_NONE, kInputRest);
    s.output = CreateTexture(w.device.Get(), kW, kH, answerFormat, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, kOutputRest);
    const bool ok = s.color && s.depth && s.motion && s.answer && s.output;
    Check(ok, "transfer textures allocate: " + s.tag);
    s.input = ofps::sdk::DefaultInputDescriptionV2(kW, kH);
    return ok;
}

inline ofps::sdk::D3D12SourceResources SceneSources(const TransferScene &s, ID3D12Resource *color) {
    return {{color, s.frameFormat}, {s.depth.Get(), DXGI_FORMAT_R32_FLOAT},
            {s.motion.Get(), DXGI_FORMAT_R16G16_FLOAT}, {nullptr, DXGI_FORMAT_UNKNOWN}};
}

// Uploads the frame and depth, packs them and reads the model input back.
inline bool PackScene(WarpDevice &w, TransferScene &s, const PixelFill &frame, const Fill &depth, bool reversed) {
    s.input.depthConvention = reversed ? ofps::sdk::DepthConvention::Reversed : ofps::sdk::DepthConvention::Normal;
    Check(BeginList(w), "begin transfer Pack list: " + s.tag);
    UploadPixels(w, s.color.Get(), kInputRest, s.frameFormat, kW, kH, frame, s.uploads);
    UploadDepth(w, s.depth.Get(), kInputRest, depth, s.uploads);
    const D3D12_RESOURCE_STATES rest[3]{kInputRest, kInputRest, kInputRest};
    const UINT subs[3]{0, 0, 0};
    std::string reason;
    const bool packed = s.path->Pack(w.list.Get(), 0, SceneSources(s, s.color.Get()), s.input, rest, subs, nullptr,
                                     &s.packed, reason);
    Check(packed, "transfer Pack records: " + s.tag);
    if (!packed) std::cerr << reason << '\n';
    if (packed) {
        s.packedRead = CreateReadback(w.device.Get(), s.packed.color);
        CaptureTarget(w, s.packed.color, s.path->PackedRestState(), s.packedRead);
    }
    Check(SubmitList(w) && WaitForQueue(w.device.Get(), w.queue.Get()), "transfer Pack submits: " + s.tag);
    return packed;
}

// The fake model's answer at a work texel, starting from what it was given.
inline std::array<float, 4> ModelInput(const TransferScene &s, std::uint32_t x, std::uint32_t y) {
    return ReadPixel(s.packedRead, s.packFormat, x, y);
}

// Uploads the answer, unpacks into the output and reads it back. Returns Unpack's result.
inline bool UnpackScene(WarpDevice &w, TransferScene &s, const PixelFill &answer,
                        const ofps::core::warp::TransferRequest *request, ReadbackCapture &out) {
    Check(BeginList(w), "begin transfer Unpack list: " + s.tag);
    UploadPixels(w, s.answer.Get(), kInputRest, s.answerFormat, s.layout.workWidth, s.layout.workHeight, answer, s.uploads);
    const ofps::core::warp::UnpackTarget target{s.output.Get(), s.answerFormat, kOutputRest, 0, 0, 0, kW, kH, false};
    std::string reason;
    const bool unpacked = s.path->Unpack(w.list.Get(), 0, s.answer.Get(), s.answerFormat, target, 0, 1.0f, 1.0f,
                                         nullptr, request, reason);
    if (!unpacked) std::cerr << reason << '\n';
    out = CreateReadback(w.device.Get(), s.output.Get());
    CaptureTarget(w, s.output.Get(), kOutputRest, out);
    Check(SubmitList(w) && WaitForQueue(w.device.Get(), w.queue.Get()), "transfer Unpack submits: " + s.tag);
    return unpacked;
}
} // namespace coretest::transfer
