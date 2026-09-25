#include "core/api/ofps_ui_source.h"

#include <cmath>
#include <cstdlib>
#include <set>
#include <string>

using namespace ofps::ui;

static void Require(bool condition) {
    if (!condition) std::abort();
}

static bool directHostAppeared = false;
static bool DirectHostNow() { return directHostAppeared; }

struct FakeCore final : IOfpsCore {
    OfpsSettingsValues values{};
    unsigned writes = 0;
    bool invalidRange = false;
    FakeCore() {
        values.size = sizeof(values);
        values.count = OFPS_SET_COUNT;
        for (std::uint32_t id = 0; id < OFPS_SET_COUNT; ++id)
            values.v[id] = kOfpsSettings[id].defaultValue;
    }
    int CreateFeature(ID3D12GraphicsCommandList *, const OfpsFeatureDesc *, IOfpsModelHost *, IOfpsFeature **) override { return OFPS_E_STATE; }
    int AdoptFeature(ID3D12GraphicsCommandList *, const OfpsFeatureDesc *, void *, IOfpsModelHost *, IOfpsFeature **) override { return OFPS_E_STATE; }
    void NotifyForeignReleased(void *) override {}
    int SetSettings(const OfpsSettingsValues *next) override {
        values = *next;
        ++writes;
        return OFPS_OK;
    }
    void GetSettings(OfpsSettingsValues *out) override { *out = values; }
    void GetSettingRange(uint32_t id, float *lo, float *hi) override {
        if (invalidRange && id == OFPS_SET_OFFSET_X) {
            *lo = std::nanf("");
            *hi = 7.0f;
            return;
        }
        *lo = id == OFPS_SET_OFFSET_X ? -7.0f : -3.0f;
        *hi = id == OFPS_SET_OFFSET_X ? 7.0f : 3.0f;
    }
    void Status(OfpsStatus *out) override { out->size = sizeof(*out); }
    uint32_t StatusLines(OfpsStatusRow *, uint32_t) override { return 0; }
    void LayoutPreview(OfpsLayoutPreview *out) override { out->size = sizeof(*out); }
    void RegisterQueue(ID3D12Device *, ID3D12CommandQueue *) override {}
    void UnregisterQueue(ID3D12CommandQueue *) override {}
    void OnCommandListExecuted(ID3D12CommandQueue *, ID3D12CommandList *) override {}
    void RetireResource(IUnknown *, const OfpsFencePoint *) override {}
    void SetDirectHost(IOfpsHost *, uint32_t) override {}
    void SetHostCaps(IOfpsHost *, const OfpsHostCaps *) override {}
    void SetHostMotionGrid(uint32_t) override {}
    void Housekeeping() override {}
    void UnregisterHost(IOfpsHost *) override {}
    void Release() override {}
};

int main() {
    std::set<std::string> keys;
    for (std::uint32_t id = 0; id < OFPS_SET_COUNT; ++id) {
        const OfpsSettingDesc &desc = kOfpsSettings[id];
        Require(desc.id == id);
        Require(desc.iniKey != nullptr && desc.iniKey[0] != 0);
        Require(keys.insert(desc.iniKey).second);
        Require(desc.group < OFPS_GROUP_COUNT);
        Require(desc.type <= OFPS_TYPE_ENUM);
        Require(std::isfinite(desc.minValue) && std::isfinite(desc.maxValue));
        Require(desc.minValue <= desc.maxValue);
        Require(desc.visibleIf.op == OFPS_VIS_ALWAYS ||
                desc.visibleIf.settingId < OFPS_SET_COUNT);
        Require((desc.flags & OFPS_FLAG_PERSISTED) == 0u ||
                (desc.flags & OFPS_FLAG_DIAGNOSTIC) == 0u);
        if (desc.type == OFPS_TYPE_ENUM) Require(desc.enumLabels != nullptr);
        if (desc.rangeFrom != 0u)
            Require(desc.type == OFPS_TYPE_FLOAT || desc.type == OFPS_TYPE_INT);
        if (desc.type == OFPS_TYPE_FLOAT)
            Require(desc.defaultValue.f >= desc.minValue &&
                    desc.defaultValue.f <= desc.maxValue);
        else
            Require(desc.defaultValue.i >= desc.minValue &&
                    desc.defaultValue.i <= desc.maxValue);
    }
    Require(keys.size() == 45u);
    Require(OFPS_SET_COUNT == 45u);
    Require(kOfpsSettings[OFPS_SET_MODE].group == OFPS_GROUP_MODE);
    Require(kOfpsSettings[OFPS_SET_TEMPORAL_MODE].group == OFPS_GROUP_TEMPORAL);
    Require(kOfpsSettings[OFPS_SET_DEBUG_WARP_PATH].group == OFPS_GROUP_DIAGNOSTICS);
    OfpsSettingDesc capRow = kOfpsSettings[OFPS_SET_MODE];
    capRow.hostCap = OFPS_CAP_MODEL_RESOLUTION;
    UiSnapshot capSnapshot{};
    Require(!Supported(capRow, capSnapshot));
    capSnapshot.caps.flags = OFPS_CAP_MODEL_RESOLUTION;
    Require(Supported(capRow, capSnapshot));

    FakeCore core;
    core.values.v[OFPS_SET_MODE].i = 0;
    OfpsHostCaps caps{};
    caps.size = sizeof(caps);
    CoreUiSource source(core, caps, false);
    UiSnapshot snapshot{};
    Require(source.Snapshot(snapshot));
    Require(snapshot.values.count == OFPS_SET_COUNT);
    Require(snapshot.ranges[OFPS_SET_OFFSET_X].available);
    Require(snapshot.ranges[OFPS_SET_OFFSET_X].lo == -7.0f);
    Require(snapshot.ranges[OFPS_SET_OFFSET_X].hi == 7.0f);
    core.invalidRange = true;
    Require(source.Snapshot(snapshot));
    Require(!snapshot.ranges[OFPS_SET_OFFSET_X].available);
    core.invalidRange = false;
    Require(!Visible(kOfpsSettings[OFPS_SET_CENTER_X], snapshot.values));
    OfpsSettingValue value{};
    value.i = 2;
    Require(source.Commit(OFPS_SET_MODE, value));
    Require(core.writes == 1);
    Require(Explicit(core.values, OFPS_SET_MODE));
    Require(Visible(kOfpsSettings[OFPS_SET_CENTER_X], core.values));
    Require(Visible(kOfpsSettings[OFPS_SET_OFFSET_X], core.values));
    value.i = 0;
    Require(!source.Commit(OFPS_SET_DEBUG_TIMING, value));
    Require(core.writes == 1);
    CoreUiSource readOnly(core, caps, true);
    Require(!readOnly.Commit(OFPS_SET_MODE, value));
    Require(core.writes == 1);
    Require(readOnly.Snapshot(snapshot) && snapshot.readOnly && snapshot.directHost);
    CoreUiSource guarded(core, caps, false, DirectHostNow);
    Require(guarded.Snapshot(snapshot) && !snapshot.readOnly);
    directHostAppeared = true;
    Require(!guarded.Commit(OFPS_SET_MODE, value));
    Require(guarded.Snapshot(snapshot) && snapshot.readOnly);
    Require(core.writes == 1);
    directHostAppeared = false;

    const auto &temporal = kOfpsSettings[OFPS_SET_TEMPORAL_MODE];
    Require(temporal.enumLabels[0] != nullptr);
    Require(temporal.enumLabels[1] != nullptr);
    Require(temporal.enumLabels[2] != nullptr);
    Require(temporal.enumLabels[3] != nullptr);
    const auto shownTemporal = ChoicesFor(temporal);
    Require(shownTemporal.count == 3);
    Require(shownTemporal.values[0] == 0 && shownTemporal.values[1] == 1 &&
            shownTemporal.values[2] == 3);
    const auto modeChoices = ChoicesFor(kOfpsSettings[OFPS_SET_MODE]);
    Require(modeChoices.count == 3 && modeChoices.values[2] == 2);

    PendingUiEdits pending;
    value.f = 5.0f;
    Require(!pending.CommitOnRelease(source, OFPS_SET_CENTER_X, value, true, true));
    Require(core.writes == 1);
    Require(pending.CommitOnRelease(source, OFPS_SET_CENTER_X, value, false, false));
    Require(core.writes == 2);
}
