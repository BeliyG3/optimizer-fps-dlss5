#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <reshade.hpp>

#include "crash_guard.h"

#include "../ngx_hook.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace pw_addon {
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
void CrashMarkerPut(ULONGLONG firstTick, ULONGLONG nowTick)
{
    HANDLE h = CreateFileW(g_crashMarkerPath, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    char text[160];
    const int len = std::snprintf(text, sizeof(text), "Optimizer FPS session: first warped frame at tick %llu, alive at tick %llu (seconds after: %llu)",
                                  static_cast<unsigned long long>(firstTick), static_cast<unsigned long long>(nowTick),
                                  static_cast<unsigned long long>((nowTick - firstTick) / 1000ull));
    DWORD n = 0; WriteFile(h, text, static_cast<DWORD>(len), &n, nullptr); CloseHandle(h);
}
// 26.21: the marker is written from the present thread and, for the very first warped frame, from the
// render thread inside the NGX hook - a crash guard that only wrote it on the next present left no
// marker at all when that first warped evaluate killed the process.
std::mutex g_crashMarkerMutex;
void CrashMarkerWrite()
{
    if (g_crashMarkerPath[0] == 0) return;
    std::scoped_lock lock(g_crashMarkerMutex);
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
    if (!g_crashGuard || pw_ngx::SafeMode()) return;
    CrashMarkerWrite();
}

void CrashGuardOnPresent()
{
    if (g_crashGuard && !pw_ngx::SafeMode() && (CrashMarkerWasWritten() || pw_ngx::GetStatus().active)) CrashMarkerWrite();
}

void CrashGuardRetry()
{
    pw_ngx::SetSafeMode(false);
    g_crashGuardTripped = false;
    CrashMarkerClear();
}

bool CrashGuardTripped()
{
    return g_crashGuardTripped;
}

void CrashGuardInit(const wchar_t *directory)
{
    int guard = 1;
    if (reshade::get_config_value(nullptr, "PeripheralWarp", "CrashGuard", guard)) g_crashGuard = guard != 0;
    swprintf_s(g_crashMarkerPath, L"%s\\optimizer-fps-dlss5.session", directory);
    if (g_crashGuard && GetFileAttributesW(g_crashMarkerPath) != INVALID_FILE_ATTRIBUTES && !CrashMarkerMeansCrash()) {
        DeleteFileW(g_crashMarkerPath); // the previous session ran on long after its first warped frame: not our crash (killed helper/game)
        reshade::log::message(reshade::log::level::info, "Optimizer FPS: crash guard - the previous session was not unloaded cleanly but had run long after its first warped frame; not treated as a crash");
    }
    if (g_crashGuard && GetFileAttributesW(g_crashMarkerPath) != INVALID_FILE_ATTRIBUTES) {
        g_crashGuardTripped = true;
        pw_ngx::SetSafeMode(true);
        reshade::log::message(reshade::log::level::warning, "Optimizer FPS: crash guard - the previous session of this game ended without unloading the add-on (optimizer-fps-dlss5.session was left behind): NR calls are forwarded untouched this session; Retry in the tab, or delete the file. CrashGuard=0 disables the guard.");
    }
}

} // namespace pw_addon
