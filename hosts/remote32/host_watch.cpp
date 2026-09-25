#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>

#include <reshade.hpp>

#include "host_watch.h"

#include "ipc.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <vector>

namespace ofps::remote {
namespace {

struct WatchedHost {
    DWORD pid = 0;
    HANDLE process = nullptr;
    wchar_t marker[MAX_PATH] = {};
};

std::vector<WatchedHost> g_hosts;
unsigned g_scanIn = 0;
HANDLE g_leaving = nullptr;

// Children of this game named like the Feed's host. The marker sits beside the host exe, where the
// installer puts optimizer-fps-dlss5.addon64.
void Scan()
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    const DWORD self = GetCurrentProcessId();
    for (BOOL ok = Process32FirstW(snapshot, &entry); ok; ok = Process32NextW(snapshot, &entry)) {
        if (entry.th32ParentProcessID != self || _wcsicmp(entry.szExeFile, L"dlss5-feed-host64.exe") != 0) continue;
        HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
        if (process == nullptr) continue;
        WatchedHost host;
        host.pid = entry.th32ProcessID;
        host.process = process;
        wchar_t path[MAX_PATH] = {};
        DWORD size = MAX_PATH;
        wchar_t *slash = nullptr;
        if (QueryFullProcessImageNameW(process, 0, path, &size) && (slash = wcsrchr(path, L'\\')) != nullptr) {
            *slash = 0;
            swprintf_s(host.marker, L"%s\\optimizer-fps-dlss5.session", path);
        }
        g_hosts.push_back(host);
    }
    CloseHandle(snapshot);
}

std::uint64_t Ticks(const FILETIME &time)
{
    return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
}

enum class Outcome { NotStopped, NoMarker, Kept, NotItsOwn, Removed };

// The pid a 2026.10 marker names ("; pid N"), 0 for an older one.
DWORD MarkerPid(const char *text)
{
    const char *p = std::strstr(text, "; pid ");
    return p != nullptr ? static_cast<DWORD>(std::strtoul(p + 6, nullptr, 10)) : 0;
}

// The host has ended. Exit code 0 is a stop by the game; the marker goes if that host wrote it (its pid
// cannot belong to anybody else while this tab holds the process handle), unless it notes a removed
// device or was written after the host ended. A marker of another process - a crash that a host in safe
// mode found, or an older add-on's without a pid - stays: the guard keeps it until Retry.
Outcome Settle(const WatchedHost &host, DWORD *codeOut)
{
    DWORD code = 1;
    GetExitCodeProcess(host.process, &code);
    *codeOut = code;
    if (code != 0 || host.marker[0] == 0) return Outcome::NotStopped;
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(host.process, &created, &exited, &kernel, &user)) return Outcome::Kept;
    HANDLE file = CreateFileW(host.marker, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return Outcome::NoMarker;
    char text[256] = {};
    DWORD read = 0;
    ReadFile(file, text, sizeof(text) - 1, &read, nullptr);
    FILETIME written{};
    const bool timed = GetFileTime(file, nullptr, nullptr, &written) != FALSE;
    CloseHandle(file);
    if (MarkerPid(text) != host.pid) return Outcome::NotItsOwn;
    if (!timed || Ticks(written) > Ticks(exited) || std::strstr(text, "device removed") != nullptr) return Outcome::Kept;
    return DeleteFileW(host.marker) ? Outcome::Removed : Outcome::Kept;
}

void EnsureLeavingEvent()
{
    if (g_leaving != nullptr) return;
    wchar_t name[96] = {};
    swprintf_s(name, kLeavingFormat, static_cast<unsigned long>(GetCurrentProcessId()));
    g_leaving = CreateEventW(nullptr, TRUE, FALSE, name);
    // A tab loaded again in the same game (ReShade re-created its add-ons) must not leave the signal of
    // its previous instance standing.
    if (g_leaving != nullptr) ResetEvent(g_leaving);
}

} // namespace

void WatchHosts()
{
    EnsureLeavingEvent();
    for (std::size_t i = 0; i < g_hosts.size();) {
        const WatchedHost &host = g_hosts[i];
        if (WaitForSingleObject(host.process, 0) != WAIT_OBJECT_0) { ++i; continue; }
        DWORD code = 0;
        const Outcome outcome = Settle(host, &code);
        char line[200];
        std::snprintf(line, sizeof(line), "Optimizer FPS remote: the host (pid %lu) ended with exit code 0x%lX; crash guard marker %s",
                      static_cast<unsigned long>(host.pid), static_cast<unsigned long>(code),
                      outcome == Outcome::Removed ? "removed (the game stopped it)"
                      : outcome == Outcome::NoMarker ? "absent"
                      : outcome == Outcome::NotItsOwn ? "kept (written by another process)"
                      : outcome == Outcome::Kept ? "kept (newer than the host, or it notes a removed device)"
                                                 : "left as it is (not a stop by the game)");
        reshade::log::message(reshade::log::level::info, line);
        CloseHandle(host.process);
        g_hosts.erase(g_hosts.begin() + static_cast<std::ptrdiff_t>(i));
        g_scanIn = 0; // the game usually starts the next host at once
    }
    // A snapshot of every process costs a little; it is taken only while no host is watched.
    if (!g_hosts.empty()) return;
    if (g_scanIn != 0) { --g_scanIn; return; }
    g_scanIn = 60;
    Scan();
}

void SignalLeaving(bool processExit)
{
    // Unloaded by ReShade (a device the game destroys): the Feed, unloaded before this tab, has just
    // killed the host, and a kill completes asynchronously - give it half a second in all. At process exit
    // the Feed stops the host only after this tab is gone: nothing to wait for.
    const ULONGLONG deadline = GetTickCount64() + (processExit ? 0 : 500);
    bool anyAlive = false;
    for (WatchedHost &host : g_hosts) {
        const ULONGLONG now = GetTickCount64();
        if (WaitForSingleObject(host.process, now < deadline ? static_cast<DWORD>(deadline - now) : 0) == WAIT_OBJECT_0) {
            DWORD code = 0;
            Settle(host, &code); // no log: ReShade may already be gone
        } else {
            anyAlive = true;
        }
        CloseHandle(host.process);
    }
    g_hosts.clear();
    // A host still running is stopped by the Feed after this tab is gone: it hears it from the event.
    if (anyAlive && g_leaving != nullptr) SetEvent(g_leaving); // the host holds its own handle
    if (g_leaving != nullptr) { CloseHandle(g_leaving); g_leaving = nullptr; }
}

} // namespace ofps::remote
