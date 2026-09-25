#include "core/api/ofps_ui_source.h"

#include <cmath>

namespace ofps::ui {

CoreUiSource::CoreUiSource(IOfpsCore &core, OfpsHostCaps caps, bool readOnly,
                           bool (*readOnlyNow)(),
                           bool (*settingAvailable)(std::uint32_t),
                           void (*persistCommit)(std::uint32_t,
                                                 const OfpsSettingsValues &))
    : core_(core), caps_(caps), readOnly_(readOnly),
      readOnlyNow_(readOnlyNow), settingAvailable_(settingAvailable),
      persistCommit_(persistCommit) {}

bool CoreUiSource::Snapshot(UiSnapshot &out) {
    out = {};
    out.values.size = sizeof(out.values);
    core_.GetSettings(&out.values);
    if (out.values.count != OFPS_SET_COUNT) return false;
    out.status.size = sizeof(out.status);
    core_.Status(&out.status);
    out.preview.size = sizeof(out.preview);
    core_.LayoutPreview(&out.preview);
    out.caps = caps_;
    out.readOnly = readOnly_ || (readOnlyNow_ && readOnlyNow_());
    out.directHost = out.readOnly;
    for (std::uint32_t id = 0; id < OFPS_SET_COUNT; ++id) {
        const OfpsSettingDesc &desc = kOfpsSettings[id];
        const bool policyAvailable = !settingAvailable_ || settingAvailable_(id) ||
                                     (desc.flags & OFPS_FLAG_DIAGNOSTIC) != 0u;
        out.available[id] = (desc.hostCap == 0u ||
                             (caps_.flags & desc.hostCap) == desc.hostCap) &&
                            policyAvailable;
        if (!out.available[id] && !policyAvailable)
            out.unavailableReason[id] = "not supported by the narrow host";
        if (desc.rangeFrom == 0u) continue;
        float lo = 0.0f;
        float hi = 0.0f;
        core_.GetSettingRange(id, &lo, &hi);
        out.ranges[id] = {lo, hi, std::isfinite(lo) &&
                                 std::isfinite(hi) && lo <= hi};
    }
    return true;
}

bool CoreUiSource::Commit(std::uint32_t id, OfpsSettingValue value) {
    if (readOnly_ || (readOnlyNow_ && readOnlyNow_()) || id >= OFPS_SET_COUNT)
        return false;
    if (id == OFPS_SET_GLOBAL_SCALE && (caps_.flags & OFPS_CAP_MODEL_RESOLUTION) != 0u)
        return false;
    const OfpsSettingDesc &desc = kOfpsSettings[id];
    if ((desc.flags & OFPS_FLAG_DIAGNOSTIC) != 0u ||
        (settingAvailable_ && !settingAvailable_(id)) ||
        (desc.hostCap != 0u && (caps_.flags & desc.hostCap) != desc.hostCap))
        return false;
    OfpsSettingsValues values{};
    values.size = sizeof(values);
    core_.GetSettings(&values);
    if (values.count != OFPS_SET_COUNT) return false;
    values.v[id] = value;
    MarkExplicit(values, id);
    if (core_.SetSettings(&values) != OFPS_OK) return false;
    values.size = sizeof(values);
    core_.GetSettings(&values);
    if (values.count == OFPS_SET_COUNT && persistCommit_)
        persistCommit_(id, values);
    return values.count == OFPS_SET_COUNT;
}

std::uint32_t CoreUiSource::StatusLines(OfpsStatusRow *rows,
                                        std::uint32_t capacity) {
    return core_.StatusLines(rows, capacity);
}

} // namespace ofps::ui
