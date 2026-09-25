#include "test_core_api_scenarios.h"
#include "test_core_api_checks.h"
#include "test_core_api_reference.h"
#include "test_core_api_half_ulp.h"

namespace coretest {
namespace {
float MeanRgbDifference(const ReadbackCapture &a, const ReadbackCapture &b) {
    if (!a.buffer || !b.buffer || a.footprint.Footprint.Width != b.footprint.Footprint.Width ||
        a.footprint.Footprint.Height != b.footprint.Footprint.Height) return -1.0f;
    void *left = nullptr, *right = nullptr;
    const D3D12_RANGE ar{0, static_cast<SIZE_T>(a.byteCount)};
    const D3D12_RANGE br{0, static_cast<SIZE_T>(b.byteCount)};
    if (FAILED(a.buffer->Map(0, &ar, &left))) return -1.0f;
    if (FAILED(b.buffer->Map(0, &br, &right))) { a.buffer->Unmap(0, nullptr); return -1.0f; }
    double sum = 0.0;
    const auto width = a.footprint.Footprint.Width, height = a.footprint.Footprint.Height;
    for (std::uint32_t y = 0; y < height; ++y) {
        const auto *pa = static_cast<const std::byte *>(left) + a.footprint.Offset +
                         static_cast<std::size_t>(y) * a.footprint.Footprint.RowPitch;
        const auto *pb = static_cast<const std::byte *>(right) + b.footprint.Offset +
                         static_cast<std::size_t>(y) * b.footprint.Footprint.RowPitch;
        for (std::uint32_t x = 0; x < width; ++x)
            for (std::size_t c = 0; c < 3; ++c) {
                std::uint16_t va, vb;
                std::memcpy(&va, pa + x * 8 + c * 2, 2);
                std::memcpy(&vb, pb + x * 8 + c * 2, 2);
                sum += std::abs(HalfToFloat(va) - HalfToFloat(vb));
            }
    }
    const D3D12_RANGE noWrite{0, 0};
    b.buffer->Unmap(0, &noWrite);
    a.buffer->Unmap(0, &noWrite);
    return static_cast<float>(sum / (static_cast<double>(width) * height * 3));
}
std::size_t ByteDifferences(const ReadbackCapture &a, const ReadbackCapture &b, std::size_t pixelBytes) {
    if (!a.buffer || !b.buffer || a.footprint.Footprint.Width != b.footprint.Footprint.Width ||
        a.footprint.Footprint.Height != b.footprint.Footprint.Height) return SIZE_MAX;
    void *left = nullptr, *right = nullptr;
    const D3D12_RANGE ar{0, static_cast<SIZE_T>(a.byteCount)};
    const D3D12_RANGE br{0, static_cast<SIZE_T>(b.byteCount)};
    if (FAILED(a.buffer->Map(0, &ar, &left))) return SIZE_MAX;
    if (FAILED(b.buffer->Map(0, &br, &right))) { a.buffer->Unmap(0, nullptr); return SIZE_MAX; }
    std::size_t changed = 0;
    std::size_t reported = 0;
    std::size_t computeHigher = 0, pixelHigher = 0;
    for (std::uint32_t y = 0; y < a.footprint.Footprint.Height; ++y) {
        const auto *pa = static_cast<const std::byte *>(left) + a.footprint.Offset +
                         static_cast<std::size_t>(y) * a.footprint.Footprint.RowPitch;
        const auto *pb = static_cast<const std::byte *>(right) + b.footprint.Offset +
                         static_cast<std::size_t>(y) * b.footprint.Footprint.RowPitch;
        for (std::size_t i = 0; i < a.footprint.Footprint.Width * pixelBytes; ++i) {
            if (pa[i] != pb[i]) {
                ++changed;
                const auto x = i / pixelBytes, channel = (i % pixelBytes) / 2;
                std::uint16_t va = 0, vb = 0;
                std::memcpy(&va, pa + x * pixelBytes + channel * 2, 2);
                std::memcpy(&vb, pb + x * pixelBytes + channel * 2, 2);
                if (HalfToFloat(vb) > HalfToFloat(va)) ++computeHigher;
                if (HalfToFloat(va) > HalfToFloat(vb)) ++pixelHigher;
                if (reported < 6) {
                    std::cerr << "guide mismatch " << x << ',' << y << " channel " << channel
                              << " pixel=" << HalfToFloat(va) << " compute=" << HalfToFloat(vb) << '\n';
                    ++reported;
                }
            }
        }
    }
    const D3D12_RANGE noWrite{0, 0};
    b.buffer->Unmap(0, &noWrite);
    a.buffer->Unmap(0, &noWrite);
    if (changed) std::cerr << "guide signed mismatches computeHigher=" << computeHigher
                           << " pixelHigher=" << pixelHigher << '\n';
    return changed;
}
} // namespace

void ScenarioComputeWarp(WarpDevice &w, IOfpsCore *core, HostFrame &frame) {
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support{kColorFormat,
        D3D12_FORMAT_SUPPORT1_NONE, D3D12_FORMAT_SUPPORT2_NONE};
    const bool typedColor = SUCCEEDED(w.device->CheckFeatureSupport(
        D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support))) &&
        (support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) != 0;
    Check(typedColor, "WARP supports typed UAV stores for the fixture colour format");
    if (!typedColor) return;

    auto settings = CurrentSettings(core);
    SetInt(settings, OFPS_SET_MODE, 2);
    SetFloat(settings, OFPS_SET_CENTER_X, 80.0f);
    SetFloat(settings, OFPS_SET_CENTER_Y, 80.0f);
    SetFloat(settings, OFPS_SET_WORK_X, 90.0f);
    SetFloat(settings, OFPS_SET_WORK_Y, 90.0f);
    SetInt(settings, OFPS_SET_TEMPORAL_MODE, 0);
    SetInt(settings, OFPS_SET_DEBUG_WARP_PATH, 1);
    Check(core->SetSettings(&settings) == OFPS_OK, "forced compute warp setting is accepted");
    FakeModelHost model;
    IOfpsFeature *feature = nullptr;
    Check(CreateFeatureOnList(w, core, &model, &feature) == OFPS_OK && feature,
          "compute warp feature creates");
    if (!feature) return;
    const EvalRun run = RunEvaluate(w, core, feature, frame, frame.outputPadded.Get(), 1);
    Check(run.ok && run.result == OFPS_OK && run.eval.path == OFPS_PATH_WARPED &&
          run.eval.warpPath == OFPS_WARP_COMPUTE && run.eval.modelResult == OFPS_OK,
          "WARP GPU records Pack and Unpack on compute and ends the model frame");
    Check(CurrentStatus(core).warpPath == OFPS_WARP_COMPUTE,
          "Status reports the effective compute path");
    Check(MatchesSdkReference(run.output) &&
          PixelIs(run.output, kW, kH - 1, kMarker, 1.0e-6f),
          "compute output matches the SDK reference and preserves output padding");
    Check(model.endFrameCalls == 1, "compute warp calls EndFrame once");
    SetInt(settings, OFPS_SET_TEMPORAL_MODE, 1);
    Check(core->SetSettings(&settings) == OFPS_OK, "compute temporal setting is accepted");
    const EvalRun temporal = RunEvaluate(w, core, feature, frame, frame.output.Get(), 1);
    Check(temporal.ok && temporal.result == OFPS_OK && model.endFrameCalls == 2,
          "compute temporal frame records and closes without debug-layer errors");
    const EvalRun carried = RunEvaluate(w, core, feature, frame, frame.output.Get(), 0);
    Check(carried.ok && carried.result == OFPS_OK && carried.eval.path == OFPS_PATH_CARRIED &&
          carried.eval.warpPath == OFPS_WARP_NONE && model.endFrameCalls == 3,
          "compute base Unpack records an interpolated frame and closes it");
    feature->Release();
    DrainReleasedModels(w, core);
    SetInt(settings, OFPS_SET_MODE, 1);
    SetInt(settings, OFPS_SET_TEMPORAL_MODE, 0);
    SetInt(settings, OFPS_SET_DEBUG_WARP_PATH, 2);
    Check(core->SetSettings(&settings) == OFPS_OK, "uniform pixel comparison setting is accepted");
    HostFrame large;
    Check(CreateHostFrame(w, large, 1920, 1080), "full-resolution uniform WARP frame allocates");
    if (!large.output) return;
    FakeModelHost uniformModel;
    IOfpsFeature *uniformFeature = nullptr;
    Check(CreateFeatureOnList(w, core, &uniformModel, &uniformFeature, 1920, 1080) == OFPS_OK && uniformFeature,
          "uniform comparison feature creates");
    if (uniformFeature) {
        const auto pixel = RunEvaluate(w, core, uniformFeature, large, large.output.Get(), 1);
        const auto pixelPacked = uniformModel.colorCapture;
        const auto pixelDepth = uniformModel.depthCapture;
        const auto pixelMotion = uniformModel.motionCapture;
        SetInt(settings, OFPS_SET_DEBUG_WARP_PATH, 1);
        Check(core->SetSettings(&settings) == OFPS_OK, "uniform compute comparison setting is accepted");
        const auto compute = RunEvaluate(w, core, uniformFeature, large, large.output.Get(), 1);
        const auto packedMad = MeanRgbDifference(pixelPacked, uniformModel.colorCapture);
        const auto outputMad = MeanRgbDifference(pixel.output, compute.output);
        const auto depthBytes = ByteDifferences(pixelDepth, uniformModel.depthCapture, 4);
        const auto motionBytes = ByteDifferences(pixelMotion, uniformModel.motionCapture, 4);
        const bool motionWithinThreshold = PackedMotionWithinWorkPixelThreshold(
            pixelMotion, uniformModel.motionCapture);
        std::cerr << "uniform WARP float MAD packed=" << packedMad << " output=" << outputMad
                  << " depth differing bytes=" << depthBytes << " motion differing bytes=" << motionBytes << '\n';
        Check(pixel.eval.warpPath == OFPS_WARP_PIXEL && compute.eval.warpPath == OFPS_WARP_COMPUTE &&
                  packedMad >= 0.0f && outputMad >= 0.0f,
              "uniform pixel and compute paths both evaluate with comparable readbacks");
        Check(packedMad < 0.000196f && outputMad < 0.000196f,
              "full-resolution uniform WARP no-model float MAD stays below 0.05 RGB8");
        Check(motionWithinThreshold,
              "packed motion pixel/compute differs by at most 1/64 work pixel per channel");
        uniformFeature->Release();
        DrainReleasedModels(w, core);
    }
    SetInt(settings, OFPS_SET_DEBUG_WARP_PATH, 2);
    Check(core->SetSettings(&settings) == OFPS_OK, "pixel setting is restored after compute test");
}

} // namespace coretest
