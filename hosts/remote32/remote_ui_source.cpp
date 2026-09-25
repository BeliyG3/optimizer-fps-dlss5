#define NOMINMAX
#include "remote_ui_source.h"

#include <algorithm>

namespace ofps::remote {

bool RemoteUiSource::Snapshot(ui::UiSnapshot &out) {
    if (!ofps::remote::Snapshot(snapshot_)) {
        valid_ = false;
        pending_ = false;
        return false;
    }
    if (!ValidSettings(snapshot_.applied)) {
        valid_ = false;
        pending_ = false;
        return false;
    }
    if (!pending_ || snapshot_.appliedSettingsGeneration == pendingGeneration_) {
        pending_ = false;
        local_ = snapshot_.applied;
    }
    valid_ = true;
    out = {};
    out.values = local_;
    const WireStatus &wire = snapshot_.status;
    out.status.size = sizeof(out.status);
    out.status.active = wire.active;
    out.status.featureCreated = wire.featureCreated;
    out.status.adopted = wire.adopted;
    out.status.directHost = wire.directHost;
    out.status.deviceRemoved = wire.deviceRemoved;
    out.status.nativeW = wire.nativeW; out.status.nativeH = wire.nativeH;
    out.status.workW = wire.workW; out.status.workH = wire.workH;
    out.status.temporalMode = wire.temporalMode; out.status.warpPath = wire.warpPath;
    out.status.evaluations = wire.evaluations;
    out.status.passthroughs = wire.passthroughs;
    out.status.fallbackFrames = wire.fallbackFrames;
    out.status.fullFrames = wire.fullFrames;
    out.status.interpFrames = wire.interpFrames;
    out.status.modelMs = wire.modelMs;
    out.status.modelSamples = wire.modelSamples;
    out.status.modelPassesRunning = wire.modelPassesRunning;
    out.status.lastModelResult = wire.lastModelResult;
    out.status.reason = wire.reason;
    out.status.temporalReason = wire.temporalReason;
    out.status.modelPassReason = wire.modelPassReason;
    out.status.fallbackReason = wire.fallbackReason;
    out.preview = snapshot_.preview;
    out.caps = {sizeof(OfpsHostCaps), snapshot_.hostCaps};
    out.directHost = wire.directHost != 0;
    out.readOnly = out.directHost;
    for (std::uint32_t id = 0; id < OFPS_SET_COUNT; ++id) {
        const WireRange &range = snapshot_.ranges[id];
        out.ranges[id] = {range.lo, range.hi, range.valid != 0};
        out.available[id] = snapshot_.available[id] != 0;
        out.unavailableReason[id] = snapshot_.unavailableReason[id][0]
            ? snapshot_.unavailableReason[id] : nullptr;
    }
    return true;
}

bool RemoteUiSource::Commit(std::uint32_t id, OfpsSettingValue value) {
    if (!valid_ || id >= OFPS_SET_COUNT || snapshot_.status.directHost != 0 ||
        snapshot_.available[id] == 0) return false;
    const OfpsSettingDesc &desc = kOfpsSettings[id];
    if ((desc.flags & OFPS_FLAG_DIAGNOSTIC) != 0 ||
        !HostCapSupported(snapshot_.hostCaps, desc.hostCap))
        return false;
    local_.v[id] = value;
    ui::MarkExplicit(local_, id);
    if (!PublishEdit(local_, &pendingGeneration_)) return false;
    pending_ = true;
    return true;
}

bool RemoteUiSource::CommitFeeder(std::uint32_t source, std::uint32_t grid,
                                  std::uint32_t perf) {
    if (!valid_ || !PublishFeeder(source, grid, perf, &pendingGeneration_)) return false;
    pending_ = true;
    return true;
}

std::uint32_t RemoteUiSource::StatusLines(OfpsStatusRow *rows, std::uint32_t capacity) {
    if (!valid_ || rows == nullptr) return 0;
    const std::uint32_t count = std::min({capacity, snapshot_.rowCount, kMaxRows});
    for (std::uint32_t i = 0; i < count; ++i) {
        rows[i] = {snapshot_.rows[i].group, snapshot_.rows[i].label,
                   snapshot_.rows[i].value, snapshot_.rows[i].severity};
    }
    return count;
}

} // namespace ofps::remote
