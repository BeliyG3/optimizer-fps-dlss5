#include "hosts/remote32/ipc.h"

#include <array>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <string>

using namespace ofps::remote;
void TestRemoteLink();

static void Require(bool condition) {
    if (!condition) std::abort();
}

int main() {
    BlockV4 block{};
    block.magic = kMagic;
    block.version = kVersion;
    block.size = sizeof(block);
    Require(ValidHeader(block, sizeof(block)));
    Require(!ValidHeader(block, sizeof(block) - 1u));
    block.version = 3;
    Require(!ValidHeader(block, sizeof(block)));
    block.version = kVersion;
    block.magic = 0;
    Require(!ValidHeader(block, sizeof(block)));
    block.magic = kMagic;
    block.size -= 8u;
    Require(!ValidHeader(block, sizeof(block)));
    block.size = sizeof(block);

    OfpsSettingsValues values{};
    values.size = sizeof(values);
    values.count = OFPS_SET_COUNT;
    values.v[OFPS_SET_MODE].i = 2;
    values.v[OFPS_SET_CENTER_X].f = 80.0f;
    values.explicitMask[0] = 1ull << OFPS_SET_MODE;
    for (std::uint32_t id = 0; id < OFPS_SET_COUNT; ++id)
        values.v[id].i = static_cast<int>(id);
    values.v[OFPS_SET_MODE].i = 2;
    values.v[OFPS_SET_CENTER_X].f = 80.0f;
    Require(ValidSettings(values));
    block.settings = values;
    Require(block.settings.v[OFPS_SET_MODE].i == 2);
    Require(block.settings.v[OFPS_SET_CENTER_X].f == 80.0f);
    Require(block.settings.explicitMask[0] == (1ull << OFPS_SET_MODE));
    std::uint32_t seen = 0;
    OfpsSettingsValues stable{};
    block.settingsGeneration = UINT32_MAX;
    Publish(&block.settingsSequence, block.settingsGeneration, block.settings, values);
    Require(block.settingsGeneration == 0u); // wrap is legal
    Require(ReadStable(&block.settingsSequence, block.settingsGeneration,
                       block.settings, seen, stable));
    Require(seen == 0u && stable.v[OFPS_SET_MODE].i == 2);
    for (std::uint32_t id = 0; id < OFPS_SET_COUNT; ++id)
        if (id != OFPS_SET_CENTER_X && id != OFPS_SET_MODE)
            Require(stable.v[id].i == static_cast<int>(id));
    Require(stable.explicitMask[0] == (1ull << OFPS_SET_MODE));
    block.feederEdit = {2, 4, 10};
    WireFeederEdit feeder{};
    Require(ReadEdit(block, seen, stable, feeder));
    Require(feeder.source == 2 && feeder.grid == 4 && feeder.perf == 10);
    InterlockedIncrement(&block.settingsSequence); // interrupted writer
    Require(!ReadStable(&block.settingsSequence, block.settingsGeneration,
                         block.settings, seen, stable));
    Require(!ReadEdit(block, seen, stable, feeder));
    InterlockedIncrement(&block.settingsSequence);
    values.count = OFPS_SETTINGS_MAX + 1u;
    Require(!ValidSettings(values));
    values.count = OFPS_SET_COUNT;
    values.size = 0;
    Require(!ValidSettings(values));

    std::array<char, kLineLength> text{};
    CopyText(text.data(), text.size(), nullptr);
    Require(text[0] == 0);
    CopyText(text.data(), text.size(), "ok");
    Require(std::strcmp(text.data(), "ok") == 0);
    const std::string longAscii(200u, 'x');
    CopyText(text.data(), text.size(), longAscii.c_str());
    Require(std::strlen(text.data()) == kLineLength - 1u);
    const std::string longUtf8 = std::string(94u, 'x') + "\xc3\xa9";
    CopyText(text.data(), text.size(), longUtf8.c_str());
    Require(std::strlen(text.data()) == 94u);

    OfpsStatus status{};
    status.size = sizeof(status);
    status.active = 1;
    status.nativeW = 1920;
    status.nativeH = 1080;
    status.modelPassReason = "stable";
    status.fallbackReason = "none";
    status.reason = "waiting";
    status.temporalReason = "carried";
    WireStatus copied = CopyStatus(status);
    Require(copied.active == 1);
    Require(copied.nativeW == 1920 && copied.nativeH == 1080);
    Require(std::strcmp(copied.modelPassReason, "stable") == 0);
    Require(std::strcmp(copied.fallbackReason, "none") == 0);
    Require(std::strcmp(copied.reason, "waiting") == 0);
    Require(std::strcmp(copied.temporalReason, "carried") == 0);
    status.modelPassReason = "changed after copy";
    Require(std::strcmp(copied.modelPassReason, "stable") == 0);

    OfpsStatusRow row{};
    row.group = OFPS_GROUP_TEMPORAL;
    row.label = "Full frames";
    row.value = "239";
    row.severity = 1;
    const WireRow wire = CopyRow(row);
    Require(wire.group == OFPS_GROUP_TEMPORAL);
    Require(wire.severity == 1);
    Require(std::strcmp(wire.label, "Full frames") == 0);
    Require(std::strcmp(wire.value, "239") == 0);
    Require(sizeof(BlockV4) == sizeof(block));
    Require(FreshHeartbeat(4000, 1001));
    Require(!FreshHeartbeat(4001, 1001));
    Require(!FreshHeartbeat(1, 2));
    Require(!FreshHeartbeat(1, 0));
    Require(HostCapSupported(0, 0));
    Require(!HostCapSupported(0, 0x80000000u));
    Require(HostCapSupported(0x80000000u, 0x80000000u));
    WireSnapshot published{};
    published.applied = block.settings;
    published.ranges[OFPS_SET_OFFSET_X] = {-12.0f, 12.0f, 1};
    published.ranges[OFPS_SET_WORK_SHIFT_X] = {-5.0f, 7.0f, 1};
    published.rowCount = 1;
    CopyText(published.rows[0].label, kLineLength, std::string(200, 'a').c_str());
    Publish(&block.statusSequence, block.statusGeneration, block.snapshot, published);
    WireSnapshot received{};
    Require(ReadStable(&block.statusSequence, block.statusGeneration,
                       block.snapshot, seen, received));
    Require(received.ranges[OFPS_SET_OFFSET_X].lo == -12.0f);
    Require(received.ranges[OFPS_SET_WORK_SHIFT_X].hi == 7.0f);
    Require(std::strlen(received.rows[0].label) == 95);
    InterlockedIncrement(&block.statusSequence);
    Require(!ReadStable(&block.statusSequence, block.statusGeneration,
                        block.snapshot, seen, received));
    InterlockedIncrement(&block.statusSequence);
    CopyText(text.data(), text.size(), std::string(95, 'x').c_str());
    Require(std::strlen(text.data()) == 95);
    CopyText(text.data(), text.size(), std::string(96, 'x').c_str());
    Require(std::strlen(text.data()) == 95);
    OfpsStatusRow missing{};
    const WireRow empty = CopyRow(missing);
    Require(empty.label[0] == 0 && empty.value[0] == 0);
    std::printf("layout %zu %zu %zu %zu %zu\n", sizeof(BlockV4),
                offsetof(BlockV4, settings), offsetof(BlockV4, feederEdit),
                offsetof(BlockV4, statusSequence), offsetof(BlockV4, snapshot));
    std::printf("wire %zu %zu %zu %zu %zu %zu\n", sizeof(WireStatus),
                sizeof(WireRow), sizeof(WireSnapshot),
                offsetof(WireSnapshot, rows), offsetof(WireSnapshot, ranges),
                offsetof(WireSnapshot, shell));
    TestRemoteLink();
}
