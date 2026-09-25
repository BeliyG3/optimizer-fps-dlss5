#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#ifndef OFPS_REMOTE_TEST
#include <reshade.hpp>
#endif

#include "remote_link.h"

namespace ofps::remote {
namespace {
HANDLE mapping = nullptr;
BlockV4 *block = nullptr;
HANDLE retry = nullptr;
Connection state = Connection::Missing;
WireSnapshot snapshot{};
bool valid = false;
unsigned retryIn = 0;

bool OldHostPresent() {
    HANDLE old = OpenFileMappingW(FILE_MAP_READ, FALSE, kOldMappingName);
    if (old == nullptr) return false;
    CloseHandle(old);
    return true;
}

void OpenBlock() {
    mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, kMappingName);
    if (mapping == nullptr) {
        state = OldHostPresent() ? Connection::OtherVersion : Connection::Missing;
        return;
    }
    void *view = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    MEMORY_BASIC_INFORMATION region{};
    if (view == nullptr || VirtualQuery(view, &region, sizeof(region)) == 0 ||
        region.RegionSize < sizeof(BlockV4) ||
        !ValidHeader(*static_cast<BlockV4 *>(view), region.RegionSize)) {
        if (view != nullptr) UnmapViewOfFile(view);
        CloseHandle(mapping);
        mapping = nullptr;
        state = Connection::OtherVersion;
        return;
    }
    block = static_cast<BlockV4 *>(view);
    state = Connection::Disconnected;
#ifndef OFPS_REMOTE_TEST
    reshade::log::message(reshade::log::level::info,
        "Optimizer FPS remote: connected to the Neural Rendering host process (protocol 4)");
#endif
}
} // namespace

void Close() {
    if (block != nullptr) UnmapViewOfFile(block);
    if (mapping != nullptr) CloseHandle(mapping);
    if (retry != nullptr) CloseHandle(retry);
    block = nullptr;
    mapping = nullptr;
    retry = nullptr;
    valid = false;
    state = Connection::Missing;
}

void Poll() {
    if (block == nullptr) {
        if (retryIn != 0) { --retryIn; return; }
        retryIn = 60;
        OpenBlock();
    }
    if (block == nullptr) return;
    MEMORY_BASIC_INFORMATION region{};
    if (VirtualQuery(block, &region, sizeof(region)) == 0 ||
        !ValidHeader(*block, region.RegionSize)) {
        Close();
        state = Connection::OtherVersion;
        return;
    }
    WireSnapshot next{};
    std::uint32_t generation = 0;
    if (!ReadStable(&block->statusSequence, block->statusGeneration,
                    block->snapshot, generation, next)) {
        valid = false;
        state = Connection::Disconnected;
        return;
    }
    const std::uint64_t now = GetTickCount64();
    if (!ValidSettings(next.applied) || !FreshHeartbeat(now, next.hostHeartbeatTick)) {
        Close();
        state = Connection::Disconnected;
        return;
    }
    snapshot = next;
    valid = true;
    state = Connection::Live;
    if (retry == nullptr) retry = OpenEventW(EVENT_MODIFY_STATE, FALSE, kRetryName);
}

Connection State() { return state; }

bool Snapshot(WireSnapshot &out) {
    if (!valid || state != Connection::Live) return false;
    out = snapshot;
    return true;
}

bool PublishEdit(const OfpsSettingsValues &values, std::uint32_t *generation) {
    if (!valid || state != Connection::Live || block == nullptr ||
        !ValidSettings(values) || snapshot.status.directHost != 0) return false;
    Publish(&block->settingsSequence, block->settingsGeneration, block->settings, values);
    if (generation != nullptr) *generation = block->settingsGeneration;
    return true;
}

bool PublishFeeder(std::uint32_t source, std::uint32_t grid, std::uint32_t perf,
                   std::uint32_t *generation) {
    if (!valid || state != Connection::Live || block == nullptr ||
        snapshot.status.directHost != 0 || !snapshot.shell.feederAvailable) return false;
    if ((source != 1 && source != 2) || (grid != 1 && grid != 2 && grid != 4) ||
        (perf != 5 && perf != 10 && perf != 20)) return false;
    InterlockedIncrement(&block->settingsSequence);
    if (!ValidSettings(block->settings)) block->settings = snapshot.applied;
    block->feederEdit = {source, grid, perf};
    ++block->settingsGeneration;
    InterlockedIncrement(&block->settingsSequence);
    if (generation != nullptr) *generation = block->settingsGeneration;
    return true;
}

bool RetryAvailable() { return state == Connection::Live && retry != nullptr; }
void RequestRetry() { if (RetryAvailable()) SetEvent(retry); }

} // namespace ofps::remote
