#include "hosts/remote32/remote_link.h"
#include "hosts/remote32/remote_ui_source.h"

#include <cstdio>
#include <cstring>
#include <windows.h>

namespace {
bool Await(bool (*condition)(const ofps::remote::WireSnapshot &),
           ofps::remote::WireSnapshot &snapshot, DWORD timeoutMs) {
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    do {
        ofps::remote::Poll();
        if (ofps::remote::Snapshot(snapshot) && condition(snapshot)) return true;
        Sleep(10);
    } while (GetTickCount64() < deadline);
    return false;
}
bool AnySnapshot(const ofps::remote::WireSnapshot &) { return true; }
std::uint32_t expectedGeneration = 0;
int expectedMode = 0;
bool Applied(const ofps::remote::WireSnapshot &snapshot) {
    return snapshot.appliedSettingsGeneration == expectedGeneration &&
           snapshot.applied.v[OFPS_SET_MODE].i == expectedMode;
}
std::uint32_t expectedGrid = 0;
bool FeederApplied(const ofps::remote::WireSnapshot &snapshot) {
    return snapshot.shell.feederGrid == expectedGrid;
}
bool GuardTripped(const ofps::remote::WireSnapshot &snapshot) {
    return snapshot.shell.crashGuard != 0;
}
bool GuardCleared(const ofps::remote::WireSnapshot &snapshot) {
    return snapshot.shell.crashGuard == 0;
}
}

int main(int argc, char **argv) {
    using namespace ofps::remote;
    if (argc == 2 && std::strcmp(argv[1], "--expect-other") == 0) {
        const ULONGLONG deadline = GetTickCount64() + 30000;
        while (GetTickCount64() < deadline) {
            Poll();
            if (State() == Connection::OtherVersion) {
                std::puts("PASS: old V3 host detected as another version; edits disabled");
                Close();
                return 0;
            }
            Sleep(10);
        }
        std::puts("FAIL: old V3 host not detected");
        return 1;
    }
    if (argc == 2 && std::strcmp(argv[1], "--expect-v4-only") == 0) {
        const ULONGLONG deadline = GetTickCount64() + 30000;
        while (GetTickCount64() < deadline) {
            HANDLE current = OpenFileMappingW(FILE_MAP_READ, FALSE, kMappingName);
            if (current != nullptr) {
                HANDLE old = OpenFileMappingW(FILE_MAP_READ, FALSE, kOldMappingName);
                if (old != nullptr) CloseHandle(old);
                CloseHandle(current);
                if (old != nullptr) return 1;
                std::puts("PASS: V4 mapping only; old V3 tab has no shared block");
                return 0;
            }
            Sleep(10);
        }
        std::puts("FAIL: new V4 host mapping not found");
        return 1;
    }
    if (argc == 2 && std::strcmp(argv[1], "--retry") == 0) {
        WireSnapshot guarded{};
        if (!Await(GuardTripped, guarded, 30000) || !RetryAvailable()) {
            std::puts("FAIL: guarded V4 host or Retry event missing");
            return 1;
        }
        RequestRetry();
        if (!Await(GuardCleared, guarded, 5000)) {
            std::puts("FAIL: Retry was not applied by host");
            return 1;
        }
        std::puts("PASS: Retry cleared host safe mode");
        Close();
        return 0;
    }
    WireSnapshot snapshot{};
    if (!Await(AnySnapshot, snapshot, 30000)) {
        std::puts("BLOCKED: V4 host mapping did not become live");
        return 2;
    }
    expectedMode = snapshot.applied.v[OFPS_SET_MODE].i == 0 ? 2 : 0;
    RemoteUiSource source;
    ofps::ui::UiSnapshot uiSnapshot{};
    OfpsSettingValue value{};
    value.i = expectedMode;
    expectedGeneration = snapshot.appliedSettingsGeneration + 1u;
    if (!source.Snapshot(uiSnapshot) || !source.Commit(OFPS_SET_MODE, value) ||
        !Await(Applied, snapshot, 5000)) {
        std::puts("FAIL: remote setting was not acknowledged by host");
        return 1;
    }
    std::printf("PASS edit generation %u mode %d\n", expectedGeneration, expectedMode);
    if (snapshot.shell.feederAvailable) {
        expectedGrid = snapshot.shell.feederGrid == 4 ? 2u : 4u;
        if (!source.CommitFeeder(snapshot.shell.feederSource, expectedGrid,
                                 snapshot.shell.feederPerf) ||
            !Await(FeederApplied, snapshot, 5000)) {
            std::puts("FAIL: Feeder edit was not acknowledged by host");
            return 1;
        }
        std::printf("PASS feeder grid %u\n", expectedGrid);
    } else {
        std::puts("BLOCKED: Feeder cfg unavailable in host");
        return 2;
    }
    const bool retryPresent = RetryAvailable();
    std::printf("retry event %s\n", retryPresent ? "present" : "missing");
    const ULONGLONG deadline = GetTickCount64() + 45000;
    while (State() == Connection::Live && GetTickCount64() < deadline) {
        Poll();
        Sleep(50);
    }
    const bool disconnected = State() == Connection::Disconnected;
    std::printf("host stop %s\n", disconnected ? "disconnected" : "not observed");
    Close();
    return retryPresent && disconnected ? 0 : 1;
}
