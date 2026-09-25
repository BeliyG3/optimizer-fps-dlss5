#include "core/settings/status.h"
#include <algorithm>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstring>
namespace ofps::core {
void FillStatus(const Status &in, const StatusExtras &extras, StatusStrings *strings, OfpsStatus *out) {
    OfpsStatus s{};
    s.size = sizeof(OfpsStatus);
    s.active = in.active ? 1u : 0u;
    s.featureCreated = in.featureCreated ? 1u : 0u;
    s.adopted = in.adopted ? 1u : 0u;
    s.directHost = extras.directHost;
    s.deviceRemoved = extras.deviceRemoved;
    s.nativeW = in.nativeWidth;
    s.nativeH = in.nativeHeight;
    s.workW = in.workWidth;
    s.workH = in.workHeight;
    s.temporalMode = static_cast<std::uint32_t>(in.temporalMode);
    s.warpPath = in.active ? in.warpPath : OFPS_WARP_NONE;
    s.evaluations = in.evaluations;
    s.passthroughs = in.passthroughs;
    s.fallbackFrames = in.fallbackFrames;
    s.fullFrames = in.fullFrames;
    s.interpFrames = in.interpFrames;
    s.modelMs = in.modelMsFull;
    s.modelSamples = in.modelSamplesFull;
    s.modelPassesRunning = static_cast<std::uint32_t>(std::max(in.modelPassesRunning, 0));
    s.lastModelResult = in.lastNgxResult;
    std::snprintf(strings->reason, sizeof(strings->reason), "%s", in.reason);
    std::snprintf(strings->temporalReason, sizeof(strings->temporalReason), "%s", in.temporalReason);
    std::snprintf(strings->modelPassReason, sizeof(strings->modelPassReason), "%s", in.modelPassReason);
    std::snprintf(strings->fallbackReason, sizeof(strings->fallbackReason), "%s", in.fallbackReason);
    s.reason = strings->reason;
    s.temporalReason = strings->temporalReason;
    s.modelPassReason = strings->modelPassReason;
    s.fallbackReason = strings->fallbackReason;
    const std::uint32_t size = std::min<std::uint32_t>(out->size, sizeof(OfpsStatus));
    uint32_t complete = size;
    if (size > offsetof(OfpsStatus, evaluations) && size < offsetof(OfpsStatus, modelMs))
        complete = offsetof(OfpsStatus, evaluations) +
                   ((size - offsetof(OfpsStatus, evaluations)) / sizeof(uint64_t)) * sizeof(uint64_t);
    if (size > offsetof(OfpsStatus, reason))
        complete = offsetof(OfpsStatus, reason) +
                   ((size - offsetof(OfpsStatus, reason)) / sizeof(const char *)) * sizeof(const char *);
    std::memcpy(out, &s, complete);
    out->size = size;
}
namespace {
struct RowWriter {
    StatusLineBuffer &b;
    std::uint32_t n = 0;
    void Add(std::uint32_t group, const char *label, std::uint32_t severity, const char *fmt, ...) {
        if (n >= kStatusRowsMax)
            return;
        va_list args;
        va_start(args, fmt);
        std::vsnprintf(b.text[n], kStatusRowText, fmt, args);
        va_end(args);
        b.rows[n] = OfpsStatusRow{group, label, b.text[n], severity};
        ++n;
    }
};
} // namespace
std::uint32_t BuildStatusLines(const Status &in, const TemporalSettings &t, StatusLineBuffer *buffer) {
    RowWriter w{*buffer};
    if (!in.featureCreated)
        return 0;
    w.Add(OFPS_GROUP_DIAGNOSTICS, "submissionDrops", in.submissionDrops ? 1u : 0u, "%llu",
          static_cast<unsigned long long>(in.submissionDrops));
    if (in.warpPathReason[0])
        w.Add(OFPS_GROUP_DIAGNOSTICS, "Warp path", in.warpPath == OFPS_WARP_PIXEL ? 1u : 0u,
              "%s", in.warpPathReason);
    else if (std::strstr(in.reason, "compute warp unavailable") != nullptr)
        w.Add(OFPS_GROUP_DIAGNOSTICS, "Warp path", 1u, "%s", in.reason);
    w.Add(OFPS_GROUP_MOTION_SOURCE, "Host motion", 0, "%ux%u, subrect %ux%u, MVecScale %.4f x %.4f", in.hostMotionWidth,
          in.hostMotionHeight, in.hostMotionRectWidth, in.hostMotionRectHeight, in.hostMotionScaleX,
          in.hostMotionScaleY);
    if (in.temporalFlowRequested)
        w.Add(OFPS_GROUP_MOTION_SOURCE, "Temporal optical flow", in.temporalFlowRunning ? 0u : 1u,
              "%s", in.temporalFlowRunning ? "hybrid field active" :
                  (in.temporalFlowReason[0] ? in.temporalFlowReason : "waiting for the previous submitted frame"));
    if (t.mode == 3 && in.temporalMode == 3) {
        w.Add(OFPS_GROUP_TEMPORAL, "Background model", 0,
              "(%s queue): %.1f passes/s, %.1f ms per pass, residual age %u "
              "frames, passes %llu, forced waits %llu, stalls %llu",
              in.asyncQueue, in.asyncPassesPerSecond, in.asyncModelMs, in.asyncAge,
              static_cast<unsigned long long>(in.asyncPasses), static_cast<unsigned long long>(in.asyncForcedWaits),
              static_cast<unsigned long long>(in.asyncStalls));
        if (t.maxQueue > 0)
            w.Add(OFPS_GROUP_TEMPORAL, "GPU queue cap:", 0, "%.2f ms CPU wait per frame (smoothed), %llu frames waited",
                  in.asyncQueueWaitMs, static_cast<unsigned long long>(in.asyncQueueWaits));
    }
    if (in.modelSamplesFull > 0)
        w.Add(OFPS_GROUP_DIAGNOSTICS, "Model GPU time:", 0, "%.2f ms (n=%u)", in.modelMsFull, in.modelSamplesFull);
    if (in.temporalMode != 0) {
        const auto& stats = in.temporalStats;
        if (stats.reason)
            w.Add(OFPS_GROUP_TEMPORAL, "Temporal stats", 1, "%s", stats.reason);
        else {
            const auto addRegion = [&](const char* label, const ofps::core::temporal::AcceptanceRegion& r) {
                w.Add(OFPS_GROUP_TEMPORAL, label, 0, "W/R/NR/NF %.2f/%.2f/%.2f/%.2f%%",
                      100 * r.weight, 100 * r.rejected, 100 * r.noRing, 100 * r.noFill);
            };
            addRegion("Acceptance all", stats.all);
            addRegion("Acceptance centre", stats.centre);
            addRegion("Acceptance edges", stats.edges);
            if (in.temporalGuideProbes) {
                static constexpr const char* names[] = {"rawMV.x", "rawMV.y", "depth", "depthF",
                    "chain.x", "chain.y", "fetch.x", "fetch.y", "edge", "mismatch", "expected", "linkPx"};
                for (unsigned i = 0; i < stats.guides.size(); ++i) {
                    const auto& guide = stats.guides[i];
                    w.Add(OFPS_GROUP_TEMPORAL, names[i], guide.invalid ? 1u : 0u,
                          "min %.5g max %.5g mean %.5g finite %u invalid %u",
                          guide.low, guide.high, guide.mean, guide.finite, guide.invalid);
                }
            }
        }
    }
    if (in.temporalMode != 0 && in.temporalTimingEnabled && in.temporalPassTiming.reason == nullptr) {
        for (unsigned pass = 0; pass < pwtemporalcontract::kPassCount; ++pass) {
            if (in.temporalPassTiming.samples[pass] == 0) continue;
            w.Add(OFPS_GROUP_DIAGNOSTICS, pwtemporalcontract::passes[pass].name, 0,
                  "GPU %.3f ms (n=%u)", in.temporalPassTiming.milliseconds[pass],
                  in.temporalPassTiming.samples[pass]);
        }
    } else if (in.temporalMode != 0 && in.temporalTimingEnabled && in.temporalPassTiming.reason) {
        w.Add(OFPS_GROUP_DIAGNOSTICS, "Temporal pass timing", 1, "%s", in.temporalPassTiming.reason);
    }
    if (t.modelPasses > 1)
        w.Add(OFPS_GROUP_MODEL_PASSES, "Model pass evaluations", in.modelPassesRunning < t.modelPasses ? 1u : 0u,
              "%llu / %llu / %llu", static_cast<unsigned long long>(in.modelPassEvaluations[0]),
              static_cast<unsigned long long>(in.modelPassEvaluations[1]),
              static_cast<unsigned long long>(in.modelPassEvaluations[2]));
    if (in.fallbackFrames > 0)
        w.Add(OFPS_GROUP_DIAGNOSTICS, "Frames shown as the plain colour (a stage failed):", 1, "%llu, last: %s",
              static_cast<unsigned long long>(in.fallbackFrames), in.fallbackReason);
    return w.n;
}
namespace {
void ZoneAxisRectangles(float nativeExtent, float rawWorkExtent, float centerFraction, float offsetFraction,
                        float shiftFraction, float *centerLo, float *centerHi, float *workLo, float *workHi) {
    const float halfBand = 0.5f * centerFraction * nativeExtent;
    const float bandCenter = 0.5f * nativeExtent + offsetFraction * nativeExtent;
    const float halfSpan[2] = {bandCenter, nativeExtent - bandCenter};
    const float periphery[2] = {std::max(0.0f, halfSpan[0] - halfBand), std::max(0.0f, halfSpan[1] - halfBand)};
    const float budget = std::max(0.0f, rawWorkExtent - centerFraction * nativeExtent);
    const int narrow = periphery[0] <= periphery[1] ? 0 : 1;
    float allotted[2] = {};
    allotted[narrow] = std::min(0.5f * budget, periphery[narrow]);
    allotted[1 - narrow] = std::min(budget - allotted[narrow], periphery[1 - narrow]);
    const float maxShift = std::max(0.0f, std::min(allotted[0], periphery[1] - allotted[1]));
    const float minShift = -std::max(0.0f, std::min(allotted[1], periphery[0] - allotted[0]));
    const float shift = std::clamp(shiftFraction * nativeExtent, minShift, maxShift);
    allotted[0] = std::clamp(allotted[0] - shift, 0.0f, periphery[0]);
    allotted[1] = std::clamp(allotted[1] + shift, 0.0f, periphery[1]);
    *centerLo = bandCenter - halfBand;
    *centerHi = bandCenter + halfBand;
    *workLo = bandCenter - halfBand - allotted[0];
    *workHi = bandCenter + halfBand + allotted[1];
}
} // namespace
bool BuildLayoutPreview(const ofps::sdk::ConfigV2 &config, std::uint32_t nativeW, std::uint32_t nativeH,
                        OfpsLayoutPreview *out) {
    const std::uint32_t size = out->size;
    std::memset(out, 0, sizeof(*out));
    out->size = size;
    ofps::sdk::LayoutV2 layout{};
    if (nativeW == 0 || nativeH == 0 ||
        ofps::sdk::BuildLayout(config, nativeW, nativeH, &layout) != ofps::sdk::Status::Ok)
        return false;
    const float nw = static_cast<float>(nativeW), nh = static_cast<float>(nativeH);
    out->nativeW = nativeW;
    out->nativeH = nativeH;
    out->rawWorkW = layout.rawWorkWidth;
    out->rawWorkH = layout.rawWorkHeight;
    out->modelW = layout.workWidth;
    out->modelH = layout.workHeight;
    out->pixelPercent = layout.pixelPercent;
    out->maxSourceFootprintX = layout.maximumSourceFootprintX;
    out->maxSourceFootprintY = layout.maximumSourceFootprintY;
    out->diagnosticFlags = layout.diagnosticFlags;
    out->offsetMaxX = ofps::sdk::MaximumCenterOffsetPercentV2(config.xAxis.centerPercent);
    out->offsetMaxY = ofps::sdk::MaximumCenterOffsetPercentV2(config.yAxis.centerPercent);
    ofps::sdk::WorkShiftLimitsPercentV2(config, 0, &out->shiftMin[0], &out->shiftMax[0]);
    ofps::sdk::WorkShiftLimitsPercentV2(config, 1, &out->shiftMin[1], &out->shiftMax[1]);
    out->frame = OfpsRectF{0.0f, 0.0f, 1.0f, 1.0f};
    float cx0, cx1, wx0, wx1, cy0, cy1, wy0, wy1;
    ZoneAxisRectangles(nw, static_cast<float>(layout.rawWorkWidth), config.xAxis.centerPercent * 0.01f,
                       config.centerOffsetXPercent * 0.01f, config.workShiftXPercent * 0.01f, &cx0, &cx1, &wx0, &wx1);
    ZoneAxisRectangles(nh, static_cast<float>(layout.rawWorkHeight), config.yAxis.centerPercent * 0.01f,
                       config.centerOffsetYPercent * 0.01f, config.workShiftYPercent * 0.01f, &cy0, &cy1, &wy0, &wy1);
    out->center = OfpsRectF{cx0 / nw, cy0 / nh, (cx1 - cx0) / nw, (cy1 - cy0) / nh};
    out->rawWork = OfpsRectF{wx0 / nw, wy0 / nh, (wx1 - wx0) / nw, (wy1 - wy0) / nh};
    out->zoneLimits = OfpsRectF{0.5f - out->offsetMaxX * 0.01f, 0.5f - out->offsetMaxY * 0.01f, out->offsetMaxX * 0.02f,
                                out->offsetMaxY * 0.02f};
    return true;
}
} // namespace ofps::core
