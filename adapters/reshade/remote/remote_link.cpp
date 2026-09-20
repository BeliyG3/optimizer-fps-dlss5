#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <reshade.hpp>

#include "remote_link.h"

#include <cstring>

namespace pw_remote {
namespace {

HANDLE g_mapping = nullptr;
void *g_view = nullptr;
// The block as this build understands it, and the older views of the same memory. Exactly one of
// the three is non-null while connected: every version moved everything after `settings`, so an
// older host's block can only be read through the frozen layouts in the header.
PwRemoteBlockV1 *g_block = nullptr;
PwRemoteBlockOfaV2 *g_blockOfa = nullptr;
PwRemoteBlockLegacyV1 *g_blockLegacy = nullptr;
unsigned int g_blockVersion = 0;
unsigned int g_framesUntilRetry = 0;

// The last status published by the host and whether it is fresh.
PwRemoteStatusV1 g_status{};
PwRemoteSettingsV1 g_applied{};
bool g_hostAlive = false;
unsigned long long g_lastHeartbeat = 0;

// The values shown by the controls: adopted from the host once, then owned by the overlay.
PwRemoteSettingsV1 g_local{};
bool g_localValid = false;

// The settings-generation counter, whichever layout the host published.
volatile LONG *SettingsGenerationPtr()
{
    if (g_block != nullptr) return reinterpret_cast<volatile LONG *>(&g_block->settingsGeneration);
    if (g_blockOfa != nullptr) return reinterpret_cast<volatile LONG *>(&g_blockOfa->settingsGeneration);
    if (g_blockLegacy != nullptr) return reinterpret_cast<volatile LONG *>(&g_blockLegacy->settingsGeneration);
    return nullptr;
}

// The host creates the block; this side only opens it, and retries while it is absent (the host
// process starts after the game and may be restarted between runs).
void EnsureBlock()
{
    // Connected is connected, whichever layout the host published: reopening the section would
    // leak the handle and the view of the older one.
    if (g_block != nullptr || g_blockOfa != nullptr || g_blockLegacy != nullptr) return;
    if (g_framesUntilRetry != 0) {
        --g_framesUntilRetry;
        return;
    }
    g_framesUntilRetry = 60;
    // This build's section first; the name versions 1 and 2 shared only if the new one is absent,
    // so a host that has not been updated yet is still reachable.
    g_mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, PW_REMOTE_MAPPING_NAME_W);
    if (g_mapping == nullptr)
        g_mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, PW_REMOTE_MAPPING_NAME_OLD_W);
    if (g_mapping == nullptr) return;
    // The whole section, not a fixed length: an older host's block is smaller than this build's
    // struct, and asking for more than the section holds fails outright.
    g_view = MapViewOfFile(g_mapping, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if (g_view == nullptr) {
        CloseHandle(g_mapping);
        g_mapping = nullptr;
        return;
    }
    const std::uint32_t *header = static_cast<const std::uint32_t *>(g_view);
    const std::uint32_t magic = header[0];
    const std::uint32_t version = header[1];
    const std::uint32_t size = header[2];
    if (magic == PW_REMOTE_MAGIC && version == PW_REMOTE_VERSION && size == sizeof(PwRemoteBlockV1)) {
        g_block = static_cast<PwRemoteBlockV1 *>(g_view);
        g_blockVersion = PW_REMOTE_VERSION;
        reshade::log::message(reshade::log::level::info,
            "Optimizer FPS remote: connected to the Neural Rendering host process (protocol 3)");
    } else if (magic == PW_REMOTE_MAGIC && version == PW_REMOTE_VERSION_OFA &&
               size == sizeof(PwRemoteBlockOfaV2)) {
        // A host from before the model passes. Everything else works; that group is hidden.
        g_blockOfa = static_cast<PwRemoteBlockOfaV2 *>(g_view);
        g_blockVersion = PW_REMOTE_VERSION_OFA;
        reshade::log::message(reshade::log::level::info,
            "Optimizer FPS remote: connected to the Neural Rendering host process (protocol 2; "
            "the model-pass controls need a newer optimizer-fps-dlss5.addon64)");
    } else if (magic == PW_REMOTE_MAGIC && version == PW_REMOTE_VERSION_LEGACY &&
               size == sizeof(PwRemoteBlockLegacyV1)) {
        // An older host add-on. Everything the versions share works; the optical-flow group is
        // hidden, because that host has no way to act on it.
        g_blockLegacy = static_cast<PwRemoteBlockLegacyV1 *>(g_view);
        g_blockVersion = PW_REMOTE_VERSION_LEGACY;
        reshade::log::message(reshade::log::level::info,
            "Optimizer FPS remote: connected to the Neural Rendering host process (protocol 1; "
            "the optical flow controls need a newer optimizer-fps-dlss5.addon64)");
    } else {
        reshade::log::message(reshade::log::level::warning,
            "Optimizer FPS remote: the shared block does not match this add-on's version");
        Close();
        return;
    }
}

// Widens an older status/settings pair into this build's structs. The fields the versions share are
// laid out identically, so this is a prefix copy with the newer tail left at zero.
void AdoptLegacy(const PwRemoteBlockLegacyV1 &block)
{
    std::memset(&g_status, 0, sizeof(g_status));
    std::memcpy(&g_status, &block.status, sizeof(PwRemoteStatusLegacyV1));
    g_status.ofaAvailable = 0;
    g_status.ofaActive = -1;
    g_status.ofaGrid = 0;
    g_status.ofaPerf = 0;
    std::memset(&g_applied, 0, sizeof(g_applied));
    std::memcpy(&g_applied, &block.applied, sizeof(PwRemoteSettingsLegacyV1));
}

void AdoptOfaV2(const PwRemoteBlockOfaV2 &block)
{
    std::memset(&g_status, 0, sizeof(g_status));
    std::memcpy(&g_status, &block.status, sizeof(PwRemoteStatusOfaV2));
    std::memset(&g_applied, 0, sizeof(g_applied));
    std::memcpy(&g_applied, &block.applied, sizeof(PwRemoteSettingsOfaV2));
}

} // namespace

void Close()
{
    if (g_view != nullptr) UnmapViewOfFile(g_view);
    if (g_mapping != nullptr) CloseHandle(g_mapping);
    g_view = nullptr;
    g_block = nullptr;
    g_blockOfa = nullptr;
    g_blockLegacy = nullptr;
    g_blockVersion = 0;
    g_mapping = nullptr;
    g_localValid = false;
    g_hostAlive = false;
}

void Poll()
{
    EnsureBlock();
    if (!Connected()) {
        g_hostAlive = false;
        return;
    }
    if (g_block != nullptr) {
        std::memcpy(&g_status, &g_block->status, sizeof(g_status));
        std::memcpy(&g_applied, &g_block->applied, sizeof(g_applied));
    } else if (g_blockOfa != nullptr) {
        AdoptOfaV2(*g_blockOfa);
    } else {
        AdoptLegacy(*g_blockLegacy);
    }
    g_status.reason[sizeof(g_status.reason) - 1] = 0;
    g_status.modelPassReason[sizeof(g_status.modelPassReason) - 1] = 0;
    g_status.temporalReason[sizeof(g_status.temporalReason) - 1] = 0;
    g_lastHeartbeat = g_block != nullptr      ? g_block->hostHeartbeatTick
                      : g_blockOfa != nullptr ? g_blockOfa->hostHeartbeatTick
                                              : g_blockLegacy->hostHeartbeatTick;
    const unsigned long long now = GetTickCount64();
    g_hostAlive = g_lastHeartbeat != 0 && now >= g_lastHeartbeat &&
                  (now - g_lastHeartbeat) < kPwRemoteHeartbeatTimeoutMs;
    if (!g_localValid && g_hostAlive) {
        g_local = g_applied;
        g_localValid = true;
    }
}

bool Connected()
{
    return g_block != nullptr || g_blockOfa != nullptr || g_blockLegacy != nullptr;
}

bool HostAlive()
{
    return g_hostAlive;
}

unsigned int Version()
{
    return g_blockVersion;
}

const PwRemoteStatusV1 &Status()
{
    return g_status;
}

PwRemoteSettingsV1 &Local()
{
    return g_local;
}

bool LocalValid()
{
    return g_localValid;
}

void ReloadFromHost()
{
    g_local = g_applied;
    g_localValid = true;
}

void Push()
{
    if (g_block != nullptr) {
        std::memcpy(&g_block->settings, &g_local, sizeof(g_local));
    } else if (g_blockOfa != nullptr) {
        // The shared prefix only; a version-2 host has no field for the model passes.
        std::memcpy(&g_blockOfa->settings, &g_local, sizeof(PwRemoteSettingsOfaV2));
    } else if (g_blockLegacy != nullptr) {
        // The shared prefix only; a version-1 host has no field to put the rest in.
        std::memcpy(&g_blockLegacy->settings, &g_local, sizeof(PwRemoteSettingsLegacyV1));
    } else {
        return;
    }
    InterlockedIncrement(SettingsGenerationPtr());
}

} // namespace pw_remote
