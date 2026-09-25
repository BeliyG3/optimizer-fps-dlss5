#include "test_core_api_scenarios.h"
#include "test_core_api_checks.h"
#include "test_core_api_reference.h"
#include "core/warp/compute.h"

#include <string>

namespace coretest {
namespace {
using ofps::core::warp::ComputePath;
using ofps::core::warp::UnpackTarget;

bool ClearTarget(WarpDevice &w, ID3D12Resource *target, D3D12_CPU_DESCRIPTOR_HANDLE rtv,
                 D3D12_RESOURCE_STATES rest) {
    if (!target) return false;
    Transition(w.list.Get(), target, rest, D3D12_RESOURCE_STATE_RENDER_TARGET);
    w.list->ClearRenderTargetView(rtv, kMarker.data(), 0, nullptr);
    Transition(w.list.Get(), target, D3D12_RESOURCE_STATE_RENDER_TARGET, rest);
    return true;
}

bool UploadRgbaMotion(WarpDevice &w, ID3D12Resource *target,
                      std::vector<ComPtr<ID3D12Resource>> &uploads) {
    const auto footprint = CreateReadback(w.device.Get(), target);
    if (!footprint.buffer) return false;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    ComPtr<ID3D12Resource> upload;
    const auto desc = footprint.buffer->GetDesc();
    if (FAILED(w.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload)))) return false;
    void *mapped = nullptr;
    const D3D12_RANGE noRead{0, 0};
    if (FAILED(upload->Map(0, &noRead, &mapped))) return false;
    for (std::uint32_t y = 0; y < kH; ++y) {
        auto *row = static_cast<std::byte *>(mapped) + footprint.footprint.Offset +
                    static_cast<std::size_t>(y) * footprint.footprint.Footprint.RowPitch;
        for (std::uint32_t x = 0; x < kW; ++x) {
            const auto motion = Motion(x, y);
            const std::uint16_t channels[4]{
                DirectX::PackedVector::XMConvertFloatToHalf(motion.x),
                DirectX::PackedVector::XMConvertFloatToHalf(motion.y),
                DirectX::PackedVector::XMConvertFloatToHalf(0.75f),
                DirectX::PackedVector::XMConvertFloatToHalf(0.25f)};
            std::memcpy(row + x * sizeof(channels), channels, sizeof(channels));
        }
    }
    upload->Unmap(0, nullptr);
    D3D12_TEXTURE_COPY_LOCATION from{}, to{};
    from.pResource = upload.Get();
    from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    from.PlacedFootprint = footprint.footprint;
    to.pResource = target;
    to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    Transition(w.list.Get(), target, kInputRest, D3D12_RESOURCE_STATE_COPY_DEST);
    w.list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    Transition(w.list.Get(), target, D3D12_RESOURCE_STATE_COPY_DEST, kInputRest);
    uploads.push_back(upload);
    return true;
}

Image SubrectReference(const ofps::sdk::LayoutV2 &layout, std::uint32_t ox, std::uint32_t oy) {
    Image source(static_cast<std::size_t>(kW) * kH);
    for (std::uint32_t y = 0; y < kH; ++y)
        for (std::uint32_t x = 0; x < kW; ++x)
            source[y * kW + x] = Pattern(x + ox, y + oy, kPadW, kPadH);
    Image packed(static_cast<std::size_t>(layout.workWidth) * layout.workHeight);
    for (std::uint32_t y = 0; y < layout.workHeight; ++y)
        for (std::uint32_t x = 0; x < layout.workWidth; ++x) {
            auto p = Sample(source, kW, kH, ofps::sdk::UnpackPosition({x + 0.5f, y + 0.5f}, layout));
            for (auto &v : p) v = Half(v);
            packed[y * layout.workWidth + x] = p;
        }
    Image output(static_cast<std::size_t>(kW) * kH);
    for (std::uint32_t y = 0; y < kH; ++y)
        for (std::uint32_t x = 0; x < kW; ++x) {
            auto p = Sample(packed, layout.workWidth, layout.workHeight,
                            ofps::sdk::PackPosition({x + 0.5f, y + 0.5f}, layout));
            for (auto &v : p) v = Half(v);
            output[y * kW + x] = p;
        }
    return output;
}

void CaptureTarget(WarpDevice &w, ID3D12Resource *target, D3D12_RESOURCE_STATES rest,
                   const ReadbackCapture &readback) {
    Transition(w.list.Get(), target, rest, D3D12_RESOURCE_STATE_COPY_SOURCE);
    RecordReadback(w.list.Get(), target, readback);
    Transition(w.list.Get(), target, D3D12_RESOURCE_STATE_COPY_SOURCE, rest);
}

ComPtr<ID3D12Resource> ArrayAnswer(ID3D12Device *device) {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = kWorkW;
    desc.Height = kWorkH;
    desc.DepthOrArraySize = 2;
    desc.MipLevels = 1;
    desc.Format = kColorFormat;
    desc.SampleDesc.Count = 1;
    ComPtr<ID3D12Resource> result;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            kInputRest, nullptr, IID_PPV_ARGS(&result)))) result.Reset();
    return result;
}

bool SameRegion(const ReadbackCapture &a, const ReadbackCapture &b,
                std::uint32_t ox, std::uint32_t oy) {
    void *left = nullptr, *right = nullptr;
    const D3D12_RANGE ar{0, static_cast<SIZE_T>(a.byteCount)};
    const D3D12_RANGE br{0, static_cast<SIZE_T>(b.byteCount)};
    if (FAILED(a.buffer->Map(0, &ar, &left))) return false;
    if (FAILED(b.buffer->Map(0, &br, &right))) {
        a.buffer->Unmap(0, nullptr);
        return false;
    }
    bool identical = true;
    for (std::uint32_t y = oy; y < oy + kH && identical; ++y) {
        const auto *la = static_cast<const std::byte *>(left) + a.footprint.Offset +
                         static_cast<std::size_t>(y) * a.footprint.Footprint.RowPitch + ox * 8;
        const auto *rb = static_cast<const std::byte *>(right) + b.footprint.Offset +
                         static_cast<std::size_t>(y) * b.footprint.Footprint.RowPitch + ox * 8;
        identical = std::memcmp(la, rb, kW * 8) == 0;
    }
    const D3D12_RANGE noWrite{0, 0};
    b.buffer->Unmap(0, &noWrite);
    a.buffer->Unmap(0, &noWrite);
    return identical;
}
} // namespace

void ScenarioComputeDirect(WarpDevice &w, HostFrame &frame) {
    const Reference reference;
    std::string reason;
    auto path = ComputePath::Create(w.device.Get(), reference.layout, kColorFormat,
                                    kColorFormat, 1, reason);
    Check(path != nullptr, "direct WARP ComputePath creates");
    if (!path) { std::cerr << reason << '\n'; return; }

    constexpr std::uint32_t colorX = 16, colorY = 8;
    auto colorPadded = CreateTexture(w.device.Get(), kPadW, kPadH, kColorFormat,
                                    D3D12_RESOURCE_FLAG_NONE, kInputRest);
    auto motionRgba = CreateTexture(w.device.Get(), kW, kH, kColorFormat,
                                    D3D12_RESOURCE_FLAG_NONE, kInputRest);
    Check(colorPadded && motionRgba, "padded color and RGBA16F motion allocate");
    if (!colorPadded || !motionRgba) return;

    auto copied = CreateTexture(w.device.Get(), kPadW, kPadH, kColorFormat,
                                D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    auto rtv = CreateRtvHeap(w.device.Get(), 1);
    Check(copied && rtv, "non-UAV padded target allocates");
    if (!copied || !rtv) return;
    const auto copiedRtv = RtvAt(w.device.Get(), rtv.Get(), 0);
    w.device->CreateRenderTargetView(copied.Get(), nullptr, copiedRtv);
    auto directRead = CreateReadback(w.device.Get(), frame.outputPadded.Get());
    auto copiedRead = CreateReadback(w.device.Get(), copied.Get());
    auto adjustedRead = CreateReadback(w.device.Get(), frame.outputPadded.Get());
    auto adjustedCopyRead = CreateReadback(w.device.Get(), copied.Get());
    Check(directRead.buffer && copiedRead.buffer && adjustedRead.buffer && adjustedCopyRead.buffer,
          "origin target readbacks allocate");
    if (!directRead.buffer || !copiedRead.buffer || !adjustedRead.buffer || !adjustedCopyRead.buffer) return;

    ComPtr<ID3D12Fence> useFence;
    Check(SUCCEEDED(w.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&useFence))),
          "direct compute descriptor gate allocates");
    if (!useFence) return;
    const OfpsFencePoint usePoint{sizeof(OfpsFencePoint), useFence.Get(), 1};
    const D3D12_RESOURCE_STATES sourceRest[3]{kInputRest, kInputRest, kInputRest};
    const UINT subresources[3]{0, 0, 0};
    const ofps::sdk::D3D12SourceResources sources{
        {colorPadded.Get(), kColorFormat}, {frame.depth.Get(), DXGI_FORMAT_R32_FLOAT},
        {motionRgba.Get(), kColorFormat}, {nullptr, DXGI_FORMAT_UNKNOWN}};
    auto input = ofps::sdk::DefaultInputDescriptionV2(kW, kH);
    input.colorRect = {colorX, colorY, kW, kH};
    input.motionScaleX = 2.0f;
    input.motionScaleY = 3.0f;
    input.colorEncoding = ofps::sdk::ColorEncoding::LinearLdr;
    Check(BeginList(w), "begin direct Pack and Unpack list");
    std::vector<ComPtr<ID3D12Resource>> uploads;
    if (!UploadPattern(w, colorPadded.Get(), kInputRest, 0, uploads) ||
        !UploadPattern(w, frame.depth.Get(), kInputRest, 1, uploads) ||
        !UploadRgbaMotion(w, motionRgba.Get(), uploads)) {
        Check(false, "direct compute patterns upload"); return;
    }
    ofps::core::warp::Packed packed;
    const bool packedOk = path->Pack(w.list.Get(), 0, sources, input, sourceRest,
                                     subresources, &usePoint, &packed, reason);
    Check(packedOk && packed.color && packed.depth && packed.motion,
          "direct Pack records color and planar guides");
    if (!packedOk) { std::cerr << reason << '\n'; return; }

    auto packedDepth = CreateReadback(w.device.Get(), packed.depth);
    auto packedMotion = CreateReadback(w.device.Get(), packed.motion);
    Check(packedDepth.buffer && packedMotion.buffer, "planar packed guide readbacks allocate");
    if (!packedDepth.buffer || !packedMotion.buffer) return;
    CaptureTarget(w, packed.depth, path->PackedRestState(), packedDepth);
    CaptureTarget(w, packed.motion, path->PackedRestState(), packedMotion);

    constexpr std::uint32_t ox = 32, oy = 16;
    const UnpackTarget direct{frame.outputPadded.Get(), kColorFormat, kOutputRest,
                              0, ox, oy, kW, kH, false};
    const UnpackTarget copy{copied.Get(), kColorFormat,
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                            0, ox, oy, kW, kH, false};
    const auto invalidAnswer = ArrayAnswer(w.device.Get());
    Check(invalidAnswer && !path->Unpack(w.list.Get(), 0, invalidAnswer.Get(), kColorFormat,
                                        direct, 0, 1.0f, 1.0f, &usePoint, reason) &&
              reason.find("answer") != std::string::npos,
          "Unpack rejects an array answer before recording GPU work");
    auto invalidMip = direct;
    invalidMip.subresource = 1; // The one-mip, one-slice destination has no subresource 1.
    Check(!path->BaseUnpack(w.list.Get(), 0, invalidMip, &usePoint, reason) &&
              reason.find("copied") != std::string::npos,
          "Unpack rejects an absent mip or array subresource before recording GPU work");
    ClearTarget(w, frame.outputPadded.Get(), frame.rtv[4], kOutputRest);
    ClearTarget(w, copied.Get(), copiedRtv, copy.restState);
    Check(path->BaseUnpack(w.list.Get(), 0, direct, &usePoint, reason),
          "base Unpack writes shifted direct UAV region");
    Check(path->BaseUnpack(w.list.Get(), 0, copy, &usePoint, reason),
          "base Unpack writes shifted region through intermediate copy");
    CaptureTarget(w, frame.outputPadded.Get(), kOutputRest, directRead);
    CaptureTarget(w, copied.Get(), copy.restState, copiedRead);
    ClearTarget(w, frame.outputPadded.Get(), frame.rtv[4], kOutputRest);
    Check(path->Unpack(w.list.Get(), 0, packed.color, kColorFormat, direct,
                       1, 1.25f, 0.9f, &usePoint, reason),
          "Unpack records outlines, gain and gamma on the same packed slot");
    CaptureTarget(w, frame.outputPadded.Get(), kOutputRest, adjustedRead);
    ClearTarget(w, copied.Get(), copiedRtv, copy.restState);
    Check(path->Unpack(w.list.Get(), 0, packed.color, kColorFormat, copy,
                       1, 1.25f, 0.9f, &usePoint, reason),
          "Unpack records a shifted intermediate copy with adjustments");
    CaptureTarget(w, copied.Get(), copy.restState, adjustedCopyRead);
    Check(SubmitList(w) && SUCCEEDED(w.queue->Signal(useFence.Get(), 1)) &&
              WaitForQueue(w.device.Get(), w.queue.Get()),
          "direct compute submission and descriptor gate complete");
    const auto expected = SubrectReference(reference.layout, colorX, colorY);
    Check(MatchesImage(directRead, expected, kW, kH, kSdkTolerance, ox, oy),
          "shifted direct UAV matches no-model pixel reference");
    Check(MatchesImage(copiedRead, expected, kW, kH, kSdkTolerance, ox, oy),
          "shifted intermediate copy matches no-model pixel reference");
    Check(SameRegion(directRead, copiedRead, ox, oy),
          "direct UAV and intermediate copy produce byte-identical native regions");
    Check(SameRegion(adjustedRead, adjustedCopyRead, ox, oy),
          "adjusted direct UAV and intermediate copy produce byte-identical native regions");
    Check(MatchesPackedGuides(packedDepth, packedMotion),
          "RGBA16F motion .xy and planar depth match packed guide reference");
    for (const auto &capture : {directRead, copiedRead, adjustedRead, adjustedCopyRead}) {
        Check(PixelIs(capture, ox - 1, oy, kMarker, 1.0e-6f) &&
                  PixelIs(capture, ox, oy - 1, kMarker, 1.0e-6f) &&
                  PixelIs(capture, ox + kW, oy, kMarker, 1.0e-6f) &&
                  PixelIs(capture, ox, oy + kH, kMarker, 1.0e-6f),
              "Unpack leaves every edge of the padded destination untouched");
    }
    std::array<float, 4> baseCenter{}, adjustedCenter{};
    Check(ReadHalf4(directRead, ox + kW / 2, oy + kH / 2, &baseCenter) &&
              ReadHalf4(adjustedRead, ox + kW / 2, oy + kH / 2, &adjustedCenter) &&
              adjustedCenter[0] > baseCenter[0] &&
              PixelIs(adjustedRead, ox - 1, oy, kMarker, 1.0e-6f),
          "diagnostic adjustment changes interior pixels without spilling outside outputRect");
    Check(!HasDebugErrors(w.device.Get()), "direct Pack and shifted Unpack have no debug errors");
}
} // namespace coretest
