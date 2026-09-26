#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>

#include <reshade.hpp>
#include <detours.h>

#include "crash_guard.h"
#include "shell_settings.h"

#include "../shell_host.h"
#include "../ngx_hook_api.h"
#include "../../remote32/ipc.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <mutex>

namespace ofps::reshade {
namespace {

bool g_crashGuard = true;
bool g_crashGuardTripped = false; // the marker was found at start-up
bool g_crashMarkerWritten = false;
wchar_t g_crashMarkerPath[MAX_PATH] = {};
// 26.19: the marker carries how long the session lived after the first warped frame. A helper process
// (the Feed's host64) is terminated by its 32-bit side at game exit and never unloads cleanly; a game the
// user kills from the task manager does the same. Neither is our crash: at start-up a marker whose
// session ran on for more than kCrashGuardGraceSeconds after the first warped frame is discarded.
constexpr unsigned kCrashGuardGraceSeconds = 20;
ULONGLONG g_crashMarkerFirstTick = 0, g_crashMarkerLastTouch = 0;
bool g_crashMarkerGracePassed = false; // 26.26: the refresh that carries the session past the grace was written
// 2026.10: the marker also names the process that wrote it and, when that process saw its device removed,
// says so. The 32-bit tab reads both when it clears the marker of a host its game stopped (see
// remote/host_watch.cpp): only the dead host's own marker, and never one that records a removed device.
void CrashMarkerPut(ULONGLONG firstTick, ULONGLONG nowTick, bool deviceRemoved = false)
{
    HANDLE h = CreateFileW(g_crashMarkerPath, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    char text[224];
    const int len = std::snprintf(text, sizeof(text), "Optimizer FPS session: first warped frame at tick %llu, alive at tick %llu (seconds after: %llu); pid %lu%s",
                                  static_cast<unsigned long long>(firstTick), static_cast<unsigned long long>(nowTick),
                                  static_cast<unsigned long long>((nowTick - firstTick) / 1000ull),
                                  static_cast<unsigned long>(GetCurrentProcessId()), deviceRemoved ? "; device removed" : "");
    DWORD n = 0; WriteFile(h, text, static_cast<DWORD>(len), &n, nullptr); CloseHandle(h);
}
// 26.21: the marker is written from the present thread and, for the very first warped frame, from the
// render thread inside the NGX hook - a crash guard that only wrote it on the next present left no
// marker at all when that first warped evaluate killed the process.
std::mutex g_crashMarkerMutex;
// 2026.10: set once the Feed's host is leaving on its own, with exit code 0 (below). No refresh may put
// the marker back after that.
bool g_orderlyStop = false;
void CrashMarkerWrite()
{
    if (g_crashMarkerPath[0] == 0) return;
    std::scoped_lock lock(g_crashMarkerMutex);
    if (g_orderlyStop) return;
    const ULONGLONG now = GetTickCount64();
    if (!g_crashMarkerWritten) { g_crashMarkerFirstTick = now; g_crashMarkerLastTouch = now; CrashMarkerPut(now, now); g_crashMarkerWritten = true; return; }
    // 26.26: the start-up classification reads the lifetime the marker records, so the refresh cadence
    // must not leave it below the grace while the process is already past it. A slow present rate could
    // touch at 18 s and again at 24 s; a kill at 21 s then read as our crash. The first refresh at or
    // after the threshold is written whatever the cadence says; after it, every 5 s as before.
    const bool crossedGrace = !g_crashMarkerGracePassed && now - g_crashMarkerFirstTick >= kCrashGuardGraceSeconds * 1000ull;
    if (crossedGrace || now - g_crashMarkerLastTouch >= 5000) { // refreshed every 5 s while warping
        g_crashMarkerGracePassed = g_crashMarkerGracePassed || crossedGrace;
        g_crashMarkerLastTouch = now;
        CrashMarkerPut(g_crashMarkerFirstTick, now);
    }
}

// 26.26: read under the marker's own mutex (the present thread asks while the render thread may write).
bool CrashMarkerWasWritten()
{
    std::scoped_lock lock(g_crashMarkerMutex);
    return g_crashMarkerWritten;
}
// True when the marker left behind describes a session that died soon after its first warped frame.
bool CrashMarkerMeansCrash()
{
    HANDLE h = CreateFileW(g_crashMarkerPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    char text[256] = {}; DWORD n = 0; ReadFile(h, text, sizeof(text) - 1, &n, nullptr); CloseHandle(h);
    const char *p = std::strstr(text, "seconds after: ");
    if (p == nullptr) return true; // old-format marker: keep the conservative reading
    return std::strtoull(p + 15, nullptr, 10) < kCrashGuardGraceSeconds;
}

// 2026.10: the Feed's 64-bit host never unloads cleanly, so the marker survived every stop - including
// the ones the game asks for whenever settings are applied in its tab - and a stop within the grace read
// as a crash for every session after. A host leaves with exit code 0 once the game has closed the pipe
// (apply, restart, game exit), and a crash never reaches that call, so in the host an exit with code 0 on
// a device that was not removed is a clean unload. It covers the 31.08 host, whose game closes the pipe
// first. The optical-flow build's game side kills its host from outside before closing the pipe, which no
// hook in here can see; the game's 32-bit tab sees that exit code and clears the marker instead
// (remote/host_watch.cpp), and at the game's exit it signals before it goes (WatchGameLeaving below).
// Games are not covered: their exit codes promise nothing.
using PFN_ExitProcess = void(WINAPI *)(UINT);
using PFN_TerminateProcess = BOOL(WINAPI *)(HANDLE, UINT);
PFN_ExitProcess g_exitProcess = nullptr;
PFN_TerminateProcess g_terminateProcess = nullptr;

// `why` names the stop in the log: an exit with code 0, or the game's tab saying the game is leaving.
void OrderlyStop(const char *why)
{
    // The 31.08 host leaves with 0 even after its device was removed, and the newer one does when the
    // removal happened while it waited for the game. A removed device is what a warp that breaks the GPU
    // leaves behind, so then the marker stays.
    const bool removed = ofps::reshade::ShellDeviceRemoved();
    bool own = false;
    {
        std::scoped_lock lock(g_crashMarkerMutex);
        if (g_orderlyStop) return;
        g_orderlyStop = true;
        // Only this session's own marker. The one a tripped session found belongs to a crash and stays
        // until Retry: a host in safe mode that stops cleanly proves nothing about warping.
        own = g_crashMarkerPath[0] != 0 && g_crashMarkerWritten;
        if (own && !removed) DeleteFileW(g_crashMarkerPath);
        // Rewritten with the note, so the game's tab, which sees the same exit code 0, keeps it too.
        // The lifetime stays the one last recorded: time spent waiting to exit is not warped time.
        else if (own) CrashMarkerPut(g_crashMarkerFirstTick, g_crashMarkerLastTouch, true);
    }
    char line[256];
    std::snprintf(line, sizeof(line), "Optimizer FPS: crash guard - %s; %s", why,
                  !own ? "this session wrote no marker"
                  : removed ? "but the D3D12 device was removed, so the marker stays" : "marker removed");
    ::reshade::log::message(own && removed ? ::reshade::log::level::warning : ::reshade::log::level::info, line);
}

constexpr const char *kExitZero = "the host is leaving with exit code 0 (the game closed it)";

void WINAPI OrderlyExitProcess(UINT code)
{
    if (code == 0) OrderlyStop(kExitZero);
    g_exitProcess(code);
}

BOOL WINAPI OrderlyTerminateProcess(HANDLE process, UINT code)
{
    if (code == 0 && (process == GetCurrentProcess() || GetProcessId(process) == GetCurrentProcessId())) OrderlyStop(kExitZero);
    return g_terminateProcess(process, code);
}

// The game's tab signals, as ReShade unloads it, that the game is taking its Neural Rendering stack down
// (PW_REMOTE_LEAVING_EVENT_FORMAT_W, named after the game's pid - this host's parent). The Feed stops this
// host a few seconds later, by which time nobody in the game is left to see the exit code. Only a signal
// that appears while this host watches counts: one still standing from a previous tab instance does not.
bool g_feedHost = false;
DWORD g_gamePid = 0;
HANDLE g_gameLeaving = nullptr;
bool g_gameLeavingSeenClear = false, g_gameLeft = false;
unsigned g_gameLeavingLookIn = 0;

DWORD ParentProcessId()
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    DWORD parent = 0;
    for (BOOL ok = Process32FirstW(snapshot, &entry); ok; ok = Process32NextW(snapshot, &entry))
        if (entry.th32ProcessID == GetCurrentProcessId()) { parent = entry.th32ParentProcessID; break; }
    CloseHandle(snapshot);
    return parent;
}

void WatchGameLeaving()
{
    if (!g_feedHost || g_gameLeft) return;
    if (g_gameLeaving == nullptr) {
        if (g_gameLeavingLookIn != 0) { --g_gameLeavingLookIn; return; }
        g_gameLeavingLookIn = 120; // the tab makes the event on its first present; look again now and then
        if (g_gamePid == 0) g_gamePid = ParentProcessId();
        if (g_gamePid == 0) return;
        wchar_t name[96] = {};
        swprintf_s(name, ofps::remote::kLeavingFormat, static_cast<unsigned long>(g_gamePid));
        g_gameLeaving = OpenEventW(SYNCHRONIZE, FALSE, name);
        return;
    }
    if (WaitForSingleObject(g_gameLeaving, 0) != WAIT_OBJECT_0) { g_gameLeavingSeenClear = true; return; }
    if (!g_gameLeavingSeenClear) return;
    g_gameLeft = true;
    OrderlyStop("the game's tab says the game is taking its Neural Rendering down (exit, or a re-created device)");
}

bool IsProcessNamed(const wchar_t *exe)
{
    wchar_t path[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, path, MAX_PATH) == 0) return false;
    const wchar_t *name = wcsrchr(path, L'\\');
    return _wcsicmp(name != nullptr ? name + 1 : path, exe) == 0;
}

bool IsFeedHost() { return IsProcessNamed(L"dlss5-feed-host64.exe"); }

// DLSS5-Reshade-AIO's 64-bit process for 32-bit games. Its 32-bit add-on ends it from outside on every
// "apply settings" and at the game's exit, so a marker left there cannot tell a crash from a restart.
bool IsAioWrapper() { return IsProcessNamed(L"AIO DLSS5 32-bit Wrapper.exe"); }

void HookOrderlyStop()
{
    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    if (kernel == nullptr) return;
    g_exitProcess = reinterpret_cast<PFN_ExitProcess>(GetProcAddress(kernel, "ExitProcess"));
    g_terminateProcess = reinterpret_cast<PFN_TerminateProcess>(GetProcAddress(kernel, "TerminateProcess"));
    if (g_exitProcess == nullptr || g_terminateProcess == nullptr) return;
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&reinterpret_cast<PVOID &>(g_exitProcess), reinterpret_cast<PVOID>(&OrderlyExitProcess));
    DetourAttach(&reinterpret_cast<PVOID &>(g_terminateProcess), reinterpret_cast<PVOID>(&OrderlyTerminateProcess));
    const LONG error = DetourTransactionCommit();
    ::reshade::log::message(error == NO_ERROR ? ::reshade::log::level::info : ::reshade::log::level::warning,
                          error == NO_ERROR ? "Optimizer FPS: crash guard - running in the Feed's host; its exit with code 0 counts as a clean unload"
                                            : "Optimizer FPS: crash guard - could not hook the host's exit; a stop by the game can still read as a crash");
}

} // namespace

void CrashMarkerClear()
{
    std::scoped_lock lock(g_crashMarkerMutex);
    if (g_crashMarkerPath[0] != 0) DeleteFileW(g_crashMarkerPath);
    g_crashMarkerWritten = false;
    g_crashMarkerGracePassed = false;
}

void CrashMarkerOnFirstWarped()
{
    if (!g_crashGuard || ofps::reshade::SafeMode()) return;
    CrashMarkerWrite();
}

void CrashGuardOnPresent()
{
    if (g_crashGuard && !ofps::reshade::SafeMode() && (CrashMarkerWasWritten() || ([] { OfpsStatus s{}; s.size=sizeof(s); if (Core()) Core()->Status(&s); return s.active; }()))) CrashMarkerWrite();
    if (g_crashGuard) WatchGameLeaving();
}

void CrashGuardRetry()
{
    ofps::reshade::SetSafeMode(false);
    g_crashGuardTripped = false;
    CrashMarkerClear();
}

bool CrashGuardTripped()
{
    return g_crashGuardTripped;
}

void CrashGuardInit(const wchar_t *directory)
{
    g_crashGuard = CurrentShellSettings().crashGuard;
    swprintf_s(g_crashMarkerPath, L"%s\\optimizer-fps-dlss5.session", directory);
    if (!CurrentShellSettings().crashGuardPresent && IsAioWrapper()) {
        g_crashGuard = false;
        DeleteFileW(g_crashMarkerPath); // a marker from a guarded session would never be cleared
        ::reshade::log::message(::reshade::log::level::info,
            "Optimizer FPS: crash guard off in DLSS5-Reshade-AIO's 32-bit wrapper (it is ended from outside on every restart); CrashGuard=1 turns it on");
    }
    g_feedHost = IsFeedHost();
    if (g_crashGuard && !CurrentShellSettings().passive && g_feedHost) HookOrderlyStop();
    if (g_crashGuard && GetFileAttributesW(g_crashMarkerPath) != INVALID_FILE_ATTRIBUTES && !CrashMarkerMeansCrash()) {
        DeleteFileW(g_crashMarkerPath); // the previous session ran on long after its first warped frame: not our crash (killed helper/game)
        ::reshade::log::message(::reshade::log::level::info,
                                "Optimizer FPS: crash guard - the previous session was not unloaded cleanly "
                                "but had run long after its first warped frame; not treated as a crash");
    }
    if (g_crashGuard && GetFileAttributesW(g_crashMarkerPath) != INVALID_FILE_ATTRIBUTES) {
        g_crashGuardTripped = true;
        ofps::reshade::SetSafeMode(true);
        ::reshade::log::message(
            ::reshade::log::level::warning,
            "Optimizer FPS: crash guard - the previous session of this game ended without unloading the "
            "add-on (optimizer-fps-dlss5.session was left behind): NR calls are forwarded untouched this "
            "session; Retry in the tab, or delete the file. CrashGuard=0 disables the guard.");
    }
}

void CrashGuardClose()
{
    if (g_gameLeaving != nullptr) {
        CloseHandle(g_gameLeaving);
        g_gameLeaving = nullptr;
    }
}

} // namespace ofps::reshade
