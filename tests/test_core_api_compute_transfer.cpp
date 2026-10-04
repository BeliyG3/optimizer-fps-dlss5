#include "test_core_api_scenarios.h"
#include "test_core_api_transfer_scene.h"
#include "core/context.h"
#include "core/frame/warp_recorder.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <string>

namespace coretest {
namespace {
using namespace transfer;
using ofps::core::warp::TransferRequest;
using ofps::core::warp::UnpackTarget;

constexpr float kEdit = 0.125f;               // what the fake model adds
constexpr std::uint32_t kEdgeX = kW / 2 + 1;   // odd: the edge falls inside a work texel at 50 %
constexpr std::uint32_t kRow = 100;
constexpr std::uint32_t kColumnX = 200;        // even: at 50 % the Pack samples native x = 2t + 1 only

ofps::sdk::ConfigV2 UniformConfig(ofps::sdk::ColorFilter filter) {
    auto config = ofps::sdk::DefaultConfigV2();
    config.mode = ofps::sdk::WarpMode::Uniform;
    config.colorFilter = filter;
    config.xAxis.workPercent = config.yAxis.workPercent = 50.0f;
    return config;
}

const char *FilterName(ofps::sdk::ColorFilter filter) {
    return filter == ofps::sdk::ColorFilter::DetailTransferDepth ? "depth-guided" : "plain";
}

// The tests step the evaluate counter to age a Pack; it also ages deferred releases for the scenarios
// that run later in this executable, so it is given back as found (early returns included).
struct EvaluateCounterRestore {
    std::uint64_t saved = ofps::core::Ctx().evalCounter;
    ~EvaluateCounterRestore() { ofps::core::Ctx().evalCounter = saved; }
};

std::array<float, 4> WithEdit(std::array<float, 4> p) {
    for (int c = 0; c < 3; ++c) p[c] += kEdit;
    return p;
}

// Largest error across the silhouette, on the row kRow, against colour + edit on the near side.
float EdgeError(const ReadbackCapture &out) {
    float worst = 0.0f;
    for (std::uint32_t x = kEdgeX - 4; x < kEdgeX + 4; ++x) {
        std::array<float, 4> p{};
        ReadHalf4(out, x, kRow, &p);
        const float expected = Checker(x, kRow, 0) + (x < kEdgeX ? kEdit : 0.0f);
        worst = std::max(worst, std::abs(p[0] - expected));
    }
    return worst;
}

// ---- A near object left of kEdgeX gets the model's edit; the far side does not. ----
void RunCase(WarpDevice &w, ofps::sdk::ColorFilter filter, bool reversed) {
    const bool depthGuided = filter == ofps::sdk::ColorFilter::DetailTransferDepth;
    // Standard Z: near and far are close in raw depth (0.990 / 0.998) and far apart in 1/z, so only the
    // 1 - d mirror separates them.
    const float nearDepth = reversed ? 0.8f : 0.990f, farDepth = reversed ? 0.1f : 0.998f;
    TransferScene s;
    s.tag = std::string(FilterName(filter)) + (reversed ? ", reversed Z" : ", standard Z");
    if (!CreateScene(w, UniformConfig(filter), kColorFormat, kColorFormat, kColorFormat, s)) return;
    if (!PackScene(w, s, CheckerPixel,
                   [&](std::uint32_t x, std::uint32_t, int) { return x < kEdgeX ? nearDepth : farDepth; }, reversed))
        return;
    // The fake model: its answer is its input plus kEdit where the work texel's source point is on the near side.
    const auto answer = [&](std::uint32_t x, std::uint32_t y) {
        const float nativeX = ofps::sdk::UnpackPosition({x + 0.5f, y + 0.5f}, s.layout).x;
        return static_cast<std::uint32_t>(nativeX) < kEdgeX ? WithEdit(ModelInput(s, x, y)) : ModelInput(s, x, y);
    };
    const TransferRequest request{reversed};
    ReadbackCapture out;
    Check(UnpackScene(w, s, answer, &request, out) && s.path->TransferApplied(),
          "Unpack with a transfer request transfers: " + s.tag);
    std::array<float, 4> a{}, b{};
    ReadHalf4(out, 40, kRow, &a);
    ReadHalf4(out, 41, kRow, &b);
    Check(Near(a[0], Checker(40, kRow, 0) + kEdit, 0.02f) && Near(b[0], Checker(41, kRow, 0) + kEdit, 0.02f),
          "transfer keeps the one-pixel checker and adds the model edit: " + s.tag);
    ReadHalf4(out, kW - 40, kRow, &a);
    Check(Near(a[0], Checker(kW - 40, kRow, 0), 0.02f), "where the model changed nothing the frame is unchanged: " + s.tag);
    const float edge = EdgeError(out);
    std::cout << "detail transfer, " << s.tag << ": worst silhouette error " << edge << '\n';
    if (depthGuided) Check(edge < 0.02f, "depth-guided transfer does not bleed across the silhouette: " + s.tag);
    else Check(edge > 0.05f, "plain transfer bleeds across the silhouette (what depth guidance fixes): " + s.tag);
    Check(!HasDebugErrors(w.device.Get()), "transfer has no debug-layer errors: " + s.tag);

    // In-place host: the target is the colour the Pack read -> no transfer, plain unpack, no error.
    auto inPlace = CreateTexture(w.device.Get(), kW, kH, kColorFormat, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, kInputRest);
    Check(BeginList(w), "begin in-place list: " + s.tag);
    const D3D12_RESOURCE_STATES rest[3]{kInputRest, kInputRest, kInputRest};
    const UINT subs[3]{0, 0, 0};
    std::string reason;
    Check(s.path->Pack(w.list.Get(), 0, SceneSources(s, inPlace.Get()), s.input, rest, subs, nullptr, &s.packed, reason),
          "in-place Pack: " + s.tag);
    const UnpackTarget inPlaceTarget{inPlace.Get(), kColorFormat, kInputRest, 0, 0, 0, kW, kH, false};
    Check(s.path->Unpack(w.list.Get(), 0, s.answer.Get(), kColorFormat, inPlaceTarget, 0, 1.0f, 1.0f, nullptr,
                         &request, reason) && !s.path->TransferApplied(),
          "in-place target falls back to the plain unpack: " + s.tag);
    Check(SubmitList(w) && WaitForQueue(w.device.Get(), w.queue.Get()) && !HasDebugErrors(w.device.Get()),
          "in-place fallback has no debug-layer errors: " + s.tag);
}

// ---- A one-pixel column the Pack never sampled must not take the background's edit. ----
void RunThinColumn(WarpDevice &w, ofps::sdk::ColorFilter filter) {
    TransferScene s;
    s.tag = std::string(FilterName(filter)) + ", thin column";
    if (!CreateScene(w, UniformConfig(filter), kColorFormat, kColorFormat, kColorFormat, s)) return;
    // Reversed Z: background 0.7, the column 0.8 -> every tap mismatches moderately (weight ~0.013).
    if (!PackScene(w, s, CheckerPixel,
                   [](std::uint32_t x, std::uint32_t, int) { return x == kColumnX ? 0.8f : 0.7f; }, true))
        return;
    const TransferRequest request{true};
    ReadbackCapture out;
    Check(UnpackScene(w, s, [&](std::uint32_t x, std::uint32_t y) { return WithEdit(ModelInput(s, x, y)); }, &request, out) &&
              s.path->TransferApplied(), "thin-column Unpack transfers: " + s.tag);
    std::array<float, 4> column{}, background{};
    ReadHalf4(out, kColumnX, kRow, &column);
    ReadHalf4(out, kColumnX - 2, kRow, &background);
    const float native = Checker(kColumnX, kRow, 0);
    std::cout << "detail transfer, " << s.tag << ": column edit " << column[0] - native << '\n';
    Check(Near(background[0], Checker(kColumnX - 2, kRow, 0) + kEdit, 0.02f),
          "the background next to the column takes the full edit: " + s.tag);
    if (filter == ofps::sdk::ColorFilter::DetailTransferDepth)
        Check(std::abs(column[0] - native) <= 0.01f, "a column with no matching tap keeps its own colour: " + s.tag);
    else
        Check(Near(column[0], native + kEdit, 0.02f), "plain transfer gives the column the background's edit: " + s.tag);
}

// ---- A model that changes nothing must give the frame back, in any format pairing. ----
// colorFormat: the frame and the model input; answerFormat: the model's answer and the output.
void RunNoOp(WarpDevice &w, DXGI_FORMAT colorFormat, DXGI_FORMAT answerFormat, bool negatives, float tolerance,
             const char *name) {
    TransferScene s;
    s.tag = name;
    if (!CreateScene(w, UniformConfig(ofps::sdk::ColorFilter::DetailTransferDepth), colorFormat, colorFormat,
                     answerFormat, s)) return;
    const PixelFill frameValue = [&](std::uint32_t x, std::uint32_t y) {
        const float v = Checker(x, y, 0) - (negatives ? 0.5f : 0.0f);   // negatives: -0.2 / +0.1 (HDR)
        return std::array<float, 4>{v, v * 0.5f, 0.25f, static_cast<float>(x % 4) / 3.0f}; // alpha varies
    };
    if (!PackScene(w, s, frameValue, [](std::uint32_t, std::uint32_t, int) { return 0.5f; }, true)) return;
    // The answer is the model input re-written in the answer's format (a model that changes nothing).
    const TransferRequest request{true};
    ReadbackCapture out;
    Check(UnpackScene(w, s, [&](std::uint32_t x, std::uint32_t y) { return ModelInput(s, x, y); }, &request, out) &&
              s.path->TransferApplied(), "no-op Unpack transfers: " + s.tag);
    float worst = 0.0f;
    for (std::uint32_t y = 8; y < kH - 8; y += 7)
        for (std::uint32_t x = 8; x < kW - 8; ++x) {
            const auto got = ReadPixel(out, answerFormat, x, y);
            std::byte buffer[8]{};
            EncodePixel(colorFormat, frameValue(x, y), buffer);          // what the frame holds after upload
            const auto want = DecodePixel(colorFormat, buffer);
            for (int i = 0; i < 4; ++i) worst = std::max(worst, std::abs(got[i] - want[i]));
        }
    std::cout << "detail transfer no-op, " << s.tag << ": worst " << worst << " (tolerance " << tolerance << ")\n";
    Check(worst <= tolerance, "a model that changes nothing gives the frame back (alpha, sign, format): " + s.tag +
          " worst " + std::to_string(worst));
    Check(!HasDebugErrors(w.device.Get()), "no-op transfer has no debug-layer errors: " + s.tag);
}

// ---- Requests that must fall back to the plain unpack. ----
void RunNotApplied(WarpDevice &w) {
    const EvaluateCounterRestore restore;
    const TransferRequest request{true};
    const auto answer = [](std::uint32_t x, std::uint32_t y) { return WithEdit(CheckerPixel(x, y)); };
    const auto depth = [](std::uint32_t, std::uint32_t, int) { return 0.5f; };
    ReadbackCapture out;
    {
        TransferScene s;
        s.tag = "filter 1 with a transfer request";
        if (CreateScene(w, UniformConfig(ofps::sdk::ColorFilter::AdaptiveFourTap), kColorFormat, kColorFormat,
                        kColorFormat, s) && PackScene(w, s, CheckerPixel, depth, true))
            Check(UnpackScene(w, s, answer, &request, out) && !s.path->TransferApplied(), "no transfer: " + s.tag);
    }
    {
        TransferScene s;
        s.tag = "sRGB frame view, UNORM answer";
        if (CreateScene(w, UniformConfig(ofps::sdk::ColorFilter::DetailTransferDepth), DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
                        DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM, s) &&
            PackScene(w, s, CheckerPixel, depth, true))
            Check(UnpackScene(w, s, answer, &request, out) && !s.path->TransferApplied(), "no transfer: " + s.tag);
    }
    TransferScene s;
    s.tag = "answer or evaluate mismatch";
    if (!CreateScene(w, UniformConfig(ofps::sdk::ColorFilter::DetailTransferDepth), kColorFormat, kColorFormat,
                     kColorFormat, s) || !PackScene(w, s, CheckerPixel, depth, true)) return;
    Check(BeginList(w), "begin answer-is-frame list");
    const UnpackTarget target{s.output.Get(), kColorFormat, kOutputRest, 0, 0, 0, kW, kH, false};
    std::string reason;
    Check(s.path->Unpack(w.list.Get(), 0, s.color.Get(), kColorFormat, target, 0, 1.0f, 1.0f, nullptr, &request, reason) &&
              !s.path->TransferApplied(), "no transfer when the answer is the colour the Pack read");
    Check(SubmitList(w) && WaitForQueue(w.device.Get(), w.queue.Get()), "answer-is-frame list submits");
    Check(UnpackScene(w, s, answer, &request, out) && s.path->TransferApplied(),
          "the same slot transfers with its own answer in the Pack's evaluate");
    ++ofps::core::Ctx().evalCounter; // a new evaluate: the remembered host pointers may be stale
    Check(UnpackScene(w, s, answer, &request, out) && !s.path->TransferApplied(),
          "no transfer from a Pack of an earlier evaluate");
    Check(!HasDebugErrors(w.device.Get()), "fallback cases have no debug-layer errors");
}

// HDR frame: checker - 0.5 = -0.2 / +0.1.
std::array<float, 4> NegativeChecker(std::uint32_t x, std::uint32_t y) {
    const float v = Checker(x, y, 0) - 0.5f;
    return {v, v, v, 1.0f};
}

// Whole kW x kH RGBA16F readbacks, byte for byte (row padding ignored).
bool SameImage(const ReadbackCapture &a, const ReadbackCapture &b) {
    void *left = nullptr, *right = nullptr;
    const D3D12_RANGE ar{0, static_cast<SIZE_T>(a.byteCount)}, br{0, static_cast<SIZE_T>(b.byteCount)};
    if (!a.buffer || !b.buffer || FAILED(a.buffer->Map(0, &ar, &left))) return false;
    if (FAILED(b.buffer->Map(0, &br, &right))) { a.buffer->Unmap(0, nullptr); return false; }
    bool same = true;
    for (std::uint32_t y = 0; y < kH && same; ++y)
        same = std::memcmp(static_cast<const std::byte *>(left) + a.footprint.Offset + static_cast<std::size_t>(y) * a.footprint.Footprint.RowPitch,
                           static_cast<const std::byte *>(right) + b.footprint.Offset + static_cast<std::size_t>(y) * b.footprint.Footprint.RowPitch,
                           kW * 8) == 0;
    const D3D12_RANGE noWrite{0, 0};
    b.buffer->Unmap(0, &noWrite);
    a.buffer->Unmap(0, &noWrite);
    return same;
}

// ---- Without an active transfer filter 3 is filter 1's plain unpack, byte for byte (negatives clamp). ----
void RunPlainLikeFilter1(WarpDevice &w) {
    const EvaluateCounterRestore restore;
    const TransferRequest request{true};
    const char *names[3]{"filter 1, no request", "filter 3, no request", "filter 3, request declined (earlier evaluate)"};
    std::array<ReadbackCapture, 3> outs{};
    for (int i = 0; i < 3; ++i) {
        auto config = ofps::sdk::DefaultConfigV2(); // Peripheral: 1:1 band and compressed sides
        config.colorFilter = i == 0 ? ofps::sdk::ColorFilter::AdaptiveFourTap : ofps::sdk::ColorFilter::DetailTransferDepth;
        TransferScene s;
        s.tag = names[i];
        if (!CreateScene(w, config, kColorFormat, kColorFormat, kColorFormat, s) ||
            !PackScene(w, s, NegativeChecker, [](std::uint32_t, std::uint32_t, int) { return 0.5f; }, true)) return;
        if (i == 2) ++ofps::core::Ctx().evalCounter; // the request is declined: the Pack is from an earlier evaluate
        Check(UnpackScene(w, s, [&](std::uint32_t x, std::uint32_t y) { return ModelInput(s, x, y); },
                          i == 2 ? &request : nullptr, outs[i]) && !s.path->TransferApplied(),
              "plain unpack records without a transfer: " + s.tag);
    }
    Check(SameImage(outs[0], outs[1]), "filter 3 without a request is filter 1's plain unpack, byte for byte");
    Check(SameImage(outs[0], outs[2]), "filter 3 with a declined request is filter 1's plain unpack, byte for byte");
}

// ---- An active transfer keeps HDR negatives on untransferred (1:1 band) pixels too; filter 1 clamps. ----
void RunBandNegatives(WarpDevice &w) {
    const TransferRequest request{true};
    const std::uint32_t x = kW / 2 + 1, y = kH / 2; // band centre, odd x + y: checker 0.30 - 0.5 = -0.2
    for (const auto filter : {ofps::sdk::ColorFilter::AdaptiveFourTap, ofps::sdk::ColorFilter::DetailTransferDepth}) {
        auto config = ofps::sdk::DefaultConfigV2();
        config.colorFilter = filter;
        TransferScene s;
        s.tag = filter == ofps::sdk::ColorFilter::AdaptiveFourTap ? "band negatives, filter 1" : "band negatives, filter 3";
        if (!CreateScene(w, config, kColorFormat, kColorFormat, kColorFormat, s) ||
            !PackScene(w, s, NegativeChecker, [](std::uint32_t, std::uint32_t, int) { return 0.5f; }, true)) return;
        ReadbackCapture out;
        Check(UnpackScene(w, s, [&](std::uint32_t px, std::uint32_t py) { return ModelInput(s, px, py); }, &request, out),
              "band negatives Unpack records: " + s.tag);
        std::array<float, 4> p{};
        ReadHalf4(out, x, y, &p);
        const float expected = filter == ofps::sdk::ColorFilter::AdaptiveFourTap ? 0.0f : -0.2f;
        Check(Near(p[0], expected, 0.002f), "a 1:1 band pixel's HDR negative (identity gain/gamma): " + s.tag);
    }
}

// ---- The 1:1 band is the plain unpack, also next to a fractional band edge. ----
void RunBandEdge(WarpDevice &w) {
    std::array<ReadbackCapture, 2> outs{};
    // Fractional band edge 0.18 px outside the first/last band pixel's centre, and a steep periphery,
    // so that pixel's +-0.5 work-texel footprint already reaches the compressed side (1.012 > 1.001):
    // only the band test keeps the transfer off there.
    const float lo = 0.5f * kW - 0.5f * 0.574f * kW; // centred band (no offset): 136.32 .. 503.68 on 640 px
    const float hi = 0.5f * kW + 0.5f * 0.574f * kW;
    const TransferRequest request{true};
    for (int i = 0; i < 2; ++i) {
        auto config = ofps::sdk::DefaultConfigV2();
        config.colorFilter = i == 0 ? ofps::sdk::ColorFilter::AdaptiveFourTap : ofps::sdk::ColorFilter::DetailTransferDepth;
        config.xAxis = config.yAxis = {57.4f, 60.0f};
        TransferScene s;
        s.tag = i == 0 ? "band, filter 1" : "band, filter 3";
        if (!CreateScene(w, config, kColorFormat, kColorFormat, kColorFormat, s) ||
            !PackScene(w, s, CheckerPixel, [](std::uint32_t, std::uint32_t, int) { return 0.5f; }, true)) return;
        Check(UnpackScene(w, s, [](std::uint32_t x, std::uint32_t y) { return WithEdit(CheckerPixel(x, y)); },
                          i == 0 ? nullptr : &request, outs[i]), "band Unpack records: " + s.tag);
    }
    // The first and last pixel whose centre lies inside the band, on the band's own row range.
    const auto x0 = static_cast<std::uint32_t>(std::ceil(lo - 0.5f));
    const auto x1 = static_cast<std::uint32_t>(std::floor(hi - 0.5f));
    for (const std::uint32_t x : {x0, x1}) {
        std::byte a[8]{}, b[8]{};
        ReadPixelBytes(outs[0], x, kH / 2, a, 8);
        ReadPixelBytes(outs[1], x, kH / 2, b, 8);
        Check(std::memcmp(a, b, 8) == 0, "a pixel on the fractional band edge is the plain unpack, byte for byte");
    }
}

// ---- GlobalScale 70 shrinks the band too (footprint 1/0.7): the band transfers like the periphery. ----
void RunScaledBand(WarpDevice &w) {
    const TransferRequest request{true};
    const auto worstError = [](const ReadbackCapture &out, std::uint32_t x0) {
        float worst = 0.0f;
        for (std::uint32_t x = x0; x < x0 + 8; ++x) {
            std::array<float, 4> p{};
            ReadHalf4(out, x, kH / 2, &p);
            worst = std::max(worst, std::abs(p[0] - (Checker(x, kH / 2, 0) + kEdit)));
        }
        return worst;
    };
    for (const auto filter : {ofps::sdk::ColorFilter::AdaptiveFourTap, ofps::sdk::ColorFilter::DetailTransfer,
                              ofps::sdk::ColorFilter::DetailTransferDepth}) {
        auto config = ofps::sdk::DefaultConfigV2(); // Peripheral, band 80 %
        config.colorFilter = filter;
        config.globalScalePercent = 70.0f;
        const bool transfer = filter != ofps::sdk::ColorFilter::AdaptiveFourTap;
        TransferScene s;
        s.tag = std::string("GlobalScale 70, ") + (transfer ? FilterName(filter) : "filter 1");
        if (!CreateScene(w, config, kColorFormat, kColorFormat, kColorFormat, s) ||
            !PackScene(w, s, CheckerPixel, [](std::uint32_t, std::uint32_t, int) { return 0.5f; }, true)) return;
        ReadbackCapture out;
        Check(UnpackScene(w, s, [&](std::uint32_t x, std::uint32_t y) { return WithEdit(ModelInput(s, x, y)); },
                          transfer ? &request : nullptr, out) && s.path->TransferApplied() == transfer,
              "scaled-band Unpack records: " + s.tag);
        const float band = worstError(out, kW / 2 - 4), side = worstError(out, 4);
        std::cout << "detail transfer, " << s.tag << ": band error " << band << ", periphery error " << side << '\n';
        if (transfer)
            Check(band < 0.02f && side < 0.02f,
                  "the shrunk band keeps the full-size detail and adds the edit, like the periphery: " + s.tag);
        else
            Check(band > 0.05f, "filter 1 loses the band's one-pixel detail at GlobalScale 70: " + s.tag);
    }
}

// ---- The core drops the temporal base only where the transfer will apply (TransferReplacesBase). ----
void RunBaseDecision(WarpDevice &w) {
    using ofps::core::warp::TransferEligible;
    constexpr auto kSrgb = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    Check(TransferEligible(kColorFormat, kColorFormat) && TransferEligible(kSrgb, kSrgb) &&
              !TransferEligible(kSrgb, kColorFormat) && !TransferEligible(DXGI_FORMAT_R8G8B8A8_UNORM, kSrgb),
          "TransferEligible: the frame and the answer views share their sRGB-ness");
    ofps::core::FeatureState st;
    st.warpPath = ofps::core::warp::PackPath::Compute;
    st.layout.colorFilter = ofps::sdk::ColorFilter::DetailTransferDepth;
    st.colorView = st.outputView = kColorFormat;
    Check(ofps::core::TransferReplacesBase(st), "compute path, filter 3, matching views: the transfer replaces the base");
    st.colorView = kSrgb;
    Check(!ofps::core::TransferReplacesBase(st),
          "an sRGB colour view with a non-sRGB answer keeps the base (the transfer would decline)");
    st.colorView = kColorFormat;
    st.warpPath = ofps::core::warp::PackPath::Pixel;
    Check(!ofps::core::TransferReplacesBase(st), "the pixel path keeps its base (soft filter, no transfer)");
    st.warpPath = ofps::core::warp::PackPath::Compute;
    st.layout.colorFilter = ofps::sdk::ColorFilter::AdaptiveFourTap;
    Check(!ofps::core::TransferReplacesBase(st), "filter 1 keeps its base");
    // The safety net's decision (end to end unreachable: see TransferReplacesBase).
    using ofps::core::KeepResidualAfterUnpack;
    Check(!KeepResidualAfterUnpack(true, false, true),
          "a declined transfer that replaced the base leaves no residual from its frame");
    Check(KeepResidualAfterUnpack(true, true, true) && KeepResidualAfterUnpack(false, false, true) &&
              KeepResidualAfterUnpack(true, false, false) && KeepResidualAfterUnpack(false, false, false),
          "an applied transfer, no request, or a kept base keeps the frame's residual");
    // Why the pairing is not tested end to end: the core builds its compute path on the host's colour
    // and output views, and ComputePath needs typed UAV stores on both; sRGB views have none.
    ofps::sdk::LayoutV2 layout{};
    std::string reason;
    const bool built = ofps::sdk::BuildLayout(UniformConfig(ofps::sdk::ColorFilter::DetailTransferDepth), kW, kH,
                                              &layout) == ofps::sdk::Status::Ok;
    Check(built && !ofps::core::warp::ComputePath::Create(w.device.Get(), layout, kSrgb, kColorFormat, 1, reason),
          "an sRGB colour view cannot build the core's compute path on WARP");
}
} // namespace

void ScenarioComputeTransfer(WarpDevice &w) {
    for (const bool reversed : {true, false}) {
        RunCase(w, ofps::sdk::ColorFilter::DetailTransfer, reversed);
        RunCase(w, ofps::sdk::ColorFilter::DetailTransferDepth, reversed);
    }
    RunThinColumn(w, ofps::sdk::ColorFilter::DetailTransfer);
    RunThinColumn(w, ofps::sdk::ColorFilter::DetailTransferDepth);
    RunNoOp(w, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT, true, 0.002f, "RGBA16F with HDR negatives");
    RunNoOp(w, DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM, false, 2.5f / 255.0f, "R10 frame, RGBA8 answer (AIO)");
    RunNotApplied(w);
    RunPlainLikeFilter1(w);
    RunBandNegatives(w);
    RunBandEdge(w);
    RunScaledBand(w);
    RunBaseDecision(w);
}
} // namespace coretest
