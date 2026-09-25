#include "hosts/remote32/remote_link.h"
#include "hosts/remote32/remote_ui_source.h"

#include <cstdlib>
#include <cstdio>
#include <cstring>

namespace {
#define Require(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "remote link test failed at line %d: %s\n", __LINE__, #condition); \
    std::abort(); \
} } while (false)
}

void TestRemoteLink() {
    using namespace ofps::remote;
    HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
        0, static_cast<DWORD>(sizeof(BlockV4)), kMappingName);
    Require(mapping != nullptr && GetLastError() != ERROR_ALREADY_EXISTS);
    auto *block = static_cast<BlockV4 *>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS,
        0, 0, sizeof(BlockV4)));
    Require(block != nullptr);
    *block = {};
    block->magic = kMagic;
    block->version = kVersion;
    block->size = sizeof(BlockV4);
    WireSnapshot next{};
    next.applied.size = sizeof(OfpsSettingsValues);
    next.applied.count = OFPS_SET_COUNT;
    next.applied.v[OFPS_SET_MODE].i = 2;
    next.status.active = 1;
    CopyText(next.status.reason, kReasonLength, "ready");
    next.ranges[OFPS_SET_OFFSET_X] = {-11.0f, 11.0f, 1};
    next.rowCount = 1;
    CopyText(next.rows[0].label, kLineLength, "Full frames");
    CopyText(next.rows[0].value, kLineLength, "5");
    next.shell.feederAvailable = 1;
    next.available[OFPS_SET_MODE] = 1;
    next.hostHeartbeatTick = GetTickCount64();
    PublishHostSnapshot(*block, next);
    Poll();
    Require(State() == Connection::Live);
    WireSnapshot received{};
    Require(Snapshot(received) && received.status.active == 1);
    Require(received.applied.v[OFPS_SET_MODE].i == 2);
    RemoteUiSource source;
    ofps::ui::UiSnapshot uiSnapshot{};
    Require(source.Snapshot(uiSnapshot));
    Require(uiSnapshot.values.count == OFPS_SET_COUNT);
    Require(uiSnapshot.ranges[OFPS_SET_OFFSET_X].lo == -11.0f);
    Require(uiSnapshot.status.reason != nullptr &&
            std::strcmp(uiSnapshot.status.reason, "ready") == 0);
    OfpsStatusRow rows[1]{};
    Require(source.StatusLines(rows, 1) == 1);
    Require(std::strcmp(rows[0].label, "Full frames") == 0);
    OfpsSettingValue mode{};
    mode.i = 1;
    Require(source.Commit(OFPS_SET_MODE, mode));
    OfpsSettingsValues edit{};
    WireFeederEdit feeder{};
    std::uint32_t generation = 0;
    Require(ReadEdit(*block, generation, edit, feeder));
    Require(generation == 1 && edit.v[OFPS_SET_MODE].i == 1);
    Require(edit.explicitMask[0] == (1ull << OFPS_SET_MODE));
    Require(source.CommitFeeder(2, 4, 10));
    Require(ReadEdit(*block, generation, edit, feeder));
    Require(generation == 2 && feeder.source == 2 && feeder.grid == 4 && feeder.perf == 10);
    Require(edit.v[OFPS_SET_MODE].i == 1);
    next.applied = edit;
    next.appliedSettingsGeneration = generation;
    PublishHostSnapshot(*block, next);
    Poll();
    Require(source.Snapshot(uiSnapshot));
    Require(uiSnapshot.values.v[OFPS_SET_MODE].i == 1);
    next.status.directHost = 1;
    PublishHostSnapshot(*block, next);
    Poll();
    Require(source.Snapshot(uiSnapshot) && uiSnapshot.readOnly);
    Require(!source.Commit(OFPS_SET_MODE, mode));
    next.hostHeartbeatTick = GetTickCount64() - kHeartbeatTimeoutMs;
    PublishHostSnapshot(*block, next);
    Poll();
    Require(State() == Connection::Disconnected);
    Require(!PublishEdit(edit));
    UnmapViewOfFile(block);
    CloseHandle(mapping);

    HANDLE old = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
        0, 4096, kOldMappingName);
    Require(old != nullptr && GetLastError() != ERROR_ALREADY_EXISTS);
    for (int i = 0; i < 62; ++i) Poll();
    Require(State() == Connection::OtherVersion);
    Require(!PublishEdit(edit));
    CloseHandle(old);
    Close();
}
