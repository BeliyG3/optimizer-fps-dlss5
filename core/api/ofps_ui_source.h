#pragma once

#include "ofps_settings_schema.h"

#include <array>
#include <cstdint>

namespace ofps::ui {

struct SettingRange {
    float lo = 0.0f;
    float hi = 0.0f;
    bool available = false;
};

struct UiSnapshot {
    OfpsSettingsValues values{};
    OfpsStatus status{};
    OfpsLayoutPreview preview{};
    std::array<SettingRange, OFPS_SET_COUNT> ranges{};
    std::array<bool, OFPS_SET_COUNT> available{};
    std::array<const char *, OFPS_SET_COUNT> unavailableReason{};
    OfpsHostCaps caps{};
    bool readOnly = false;
    bool directHost = false;
    bool connectionLost = false;
};

class IOfpsUiSource {
public:
    virtual ~IOfpsUiSource() = default;
    virtual bool Snapshot(UiSnapshot &out) = 0;
    virtual bool Commit(std::uint32_t id, OfpsSettingValue value) = 0;
    virtual std::uint32_t StatusLines(OfpsStatusRow *rows,
                                      std::uint32_t capacity) = 0;
};

class CoreUiSource final : public IOfpsUiSource {
public:
    CoreUiSource(IOfpsCore &core, OfpsHostCaps caps, bool readOnly,
                  bool (*readOnlyNow)() = nullptr,
                  bool (*settingAvailable)(std::uint32_t) = nullptr,
                  void (*persistCommit)(std::uint32_t,
                                        const OfpsSettingsValues &) = nullptr);
    bool Snapshot(UiSnapshot &out) override;
    bool Commit(std::uint32_t id, OfpsSettingValue value) override;
    std::uint32_t StatusLines(OfpsStatusRow *rows,
                              std::uint32_t capacity) override;

private:
    IOfpsCore &core_;
    OfpsHostCaps caps_;
    bool readOnly_;
    bool (*readOnlyNow_)();
    bool (*settingAvailable_)(std::uint32_t);
    void (*persistCommit_)(std::uint32_t, const OfpsSettingsValues &);
};

class PendingUiEdits {
public:
    // Hosts with an "Advanced" switch set this; when false the Diagnostics group and the core's
    // status lines are not drawn.
    bool showAdvanced = true;

    OfpsSettingValue PendingValue(std::uint32_t id, OfpsSettingValue applied) const {
        return pendingValid_[id] ? pending_[id] : applied;
    }
    void ClearPending(std::uint32_t id) { pendingValid_[id] = false; }
    void ClearAllPending() { pendingValid_.fill(false); }
    void ClearGroupPending(std::uint32_t group) {
        for (std::uint32_t id = 0; id < OFPS_SET_COUNT; ++id)
            if (kOfpsSettings[id].group == group) pendingValid_[id] = false;
    }

    template <class Source>
    bool CommitOnRelease(Source &source, std::uint32_t id,
                         OfpsSettingValue edited, bool changed, bool active) {
        if (changed && active) {
            pending_[id] = edited;
            pendingValid_[id] = true;
            return false;
        }
        if (changed) {
            pendingValid_[id] = false;
            return source.Commit(id, edited);
        }
        if (pendingValid_[id] && !active) {
            pendingValid_[id] = false;
            return source.Commit(id, pending_[id]);
        }
        return false;
    }

private:
    std::array<OfpsSettingValue, OFPS_SET_COUNT> pending_{};
    std::array<bool, OFPS_SET_COUNT> pendingValid_{};
};

inline bool Explicit(const OfpsSettingsValues &values, std::uint32_t id) {
    return (values.explicitMask[id / 64u] & (1ull << (id % 64u))) != 0;
}

inline void MarkExplicit(OfpsSettingsValues &values, std::uint32_t id) {
    values.explicitMask[id / 64u] |= 1ull << (id % 64u);
}

inline bool Visible(const OfpsSettingDesc &desc,
                    const OfpsSettingsValues &values) {
    const OfpsVisibleIf &rule = desc.visibleIf;
    if (rule.op == OFPS_VIS_ALWAYS) return true;
    if (rule.settingId >= values.count) return false;
    const int actual = values.v[rule.settingId].i;
    switch (rule.op) {
    case OFPS_VIS_EQ: return actual == rule.value;
    case OFPS_VIS_NE: return actual != rule.value;
    case OFPS_VIS_GT: return actual > rule.value;
    default: return false;
    }
}

inline bool Supported(const OfpsSettingDesc &desc,
                      const UiSnapshot &snapshot) {
    return desc.hostCap == 0u ||
           (snapshot.caps.flags & desc.hostCap) == desc.hostCap;
}

struct EnumChoices {
    std::array<int, 16> values{};
    std::array<const char *, 16> labels{};
    int count = 0;
};

inline EnumChoices ChoicesFor(const OfpsSettingDesc &desc) {
    EnumChoices choices{};
    if (desc.enumLabels == nullptr) return choices;
    for (int id = 0; id <= static_cast<int>(desc.maxValue) &&
                     choices.count < static_cast<int>(choices.values.size()); ++id) {
        if (desc.enumLabels[id] == nullptr) break;
        if (desc.id == OFPS_SET_TEMPORAL_MODE && id == 2) continue;
        choices.values[choices.count] = id;
        choices.labels[choices.count++] = desc.enumLabels[id];
    }
    return choices;
}

} // namespace ofps::ui
