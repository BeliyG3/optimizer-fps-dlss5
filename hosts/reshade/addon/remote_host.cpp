#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstring>
#include <reshade.hpp>

#include "remote_host.h"
#include "remote_host_snapshot.h"
#include "addon_context.h"
#include "crash_guard.h"
#include "ini_store.h"
#include "config_store.h"
#include "../shell_host.h"
#include "../direct_host.h"
#include "../ngx_hook_api.h"
#include "../../remote32/ipc.h"

#include <cstdio>
#include <cstring>

namespace ofps::reshade {
namespace {
HANDLE mapping = nullptr;
ofps::remote::BlockV4 *block = nullptr;
HANDLE retry = nullptr;
bool tried = false;
std::uint32_t seenSettings = 0;
// Last settings payload taken from the remote tab. A Feeder-only edit republishes the same
// payload, so settings are applied only when it differs from the last one taken.
OfpsSettingsValues lastIncoming{};
bool lastIncomingValid = false;
ofps::feeder::Settings ofa{};
bool ofaLoaded = false;
int ofaRefreshIn = 0;
char editError[ofps::remote::kReasonLength]{};

void EnsureBlock() {
    if (block != nullptr || tried) return;
    tried = true;
    mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
        static_cast<DWORD>(sizeof(ofps::remote::BlockV4)), ofps::remote::kMappingName);
    if (mapping == nullptr) return;
    const bool existed = GetLastError() == ERROR_ALREADY_EXISTS;
    block = static_cast<ofps::remote::BlockV4 *>(
        MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(ofps::remote::BlockV4)));
    MEMORY_BASIC_INFORMATION region{};
    if (block == nullptr || VirtualQuery(block, &region, sizeof(region)) == 0 ||
        region.RegionSize < sizeof(ofps::remote::BlockV4)) {
        RemoteClose();
        return;
    }
    if (existed && !ofps::remote::ValidHeader(*block, region.RegionSize)) {
        RemoteClose();
        return;
    }
    if (!existed) std::memset(block, 0, sizeof(*block));
    block->magic = ofps::remote::kMagic;
    block->version = ofps::remote::kVersion;
    block->size = sizeof(*block);
    seenSettings = block->settingsGeneration;
    // Treat the payload already in the block as taken: a later Feeder-only edit must not replay it.
    lastIncoming = block->settings;
    lastIncomingValid = true;
    retry = CreateEventW(nullptr, FALSE, FALSE, ofps::remote::kRetryName);
    if (retry != nullptr) ResetEvent(retry);
    ::reshade::log::message(::reshade::log::level::info,
        "Optimizer FPS: remote overlay block ready (protocol 4)");
}

void ApplySettingsEdit(const OfpsSettingsValues &incoming);
void ApplyFeederEdit(const ofps::remote::WireFeederEdit &feeder);

void ApplyEdit() {
    if (block->settingsGeneration == seenSettings) return;
    OfpsSettingsValues incoming{};
    ofps::remote::WireFeederEdit feeder{};
    std::uint32_t generation = seenSettings;
    if (!ofps::remote::ReadEdit(*block, generation, incoming, feeder)) return;
    if (generation == seenSettings) return;
    seenSettings = generation;
    if (DirectHostActive() || !ofps::remote::ValidSettings(incoming) || Core() == nullptr) {
        ofps::remote::CopyText(editError, sizeof(editError),
            DirectHostActive() ? "edit rejected: applied by OptiScaler" :
            Core() == nullptr ? "edit rejected: core unavailable" : "edit rejected: invalid payload");
        return;
    }
    const bool settingsEdited = !lastIncomingValid ||
        std::memcmp(&incoming, &lastIncoming, sizeof(incoming)) != 0;
    lastIncoming = incoming;
    lastIncomingValid = true;
    if (settingsEdited) ApplySettingsEdit(incoming);
    ApplyFeederEdit(feeder);
}

void ApplySettingsEdit(const OfpsSettingsValues &incoming) {
    const int result = Core()->SetSettings(&incoming);
    if (result != OFPS_OK) {
        ofps::remote::CopyText(editError, sizeof(editError), "edit rejected by core");
        ::reshade::log::message(::reshade::log::level::warning,
            "Optimizer FPS: remote settings edit rejected by core");
        return;
    }
    editError[0] = 0;
    OfpsSettingsValues applied{};
    applied.size = sizeof(applied);
    Core()->GetSettings(&applied);
    if (!ofps::remote::ValidSettings(applied)) {
        ofps::remote::CopyText(editError, sizeof(editError),
            "edit applied but core returned invalid settings");
        return;
    }
    SaveChangedExplicitValuesToReShadeIni(applied);
    AdoptCoreValuesForOverlay();
}

void ApplyFeederEdit(const ofps::remote::WireFeederEdit &feeder) {
    if (ofaLoaded && (feeder.source == 1 || feeder.source == 2) &&
        (feeder.grid == 1 || feeder.grid == 2 || feeder.grid == 4) &&
        (feeder.perf == 5 || feeder.perf == 10 || feeder.perf == 20)) {
        ofps::feeder::Settings wanted = ofa;
        wanted.source = feeder.source == 2 ? ofps::feeder::SourceShader : ofps::feeder::SourceOfa;
        wanted.grid = static_cast<int>(feeder.grid);
        wanted.perf = static_cast<int>(feeder.perf);
        if (wanted.source != ofa.source || wanted.grid != ofa.grid || wanted.perf != ofa.perf) {
            ofa = wanted;
            OfaSave();
        }
    }
}
} // namespace

void RemoteClose() {
    if (block != nullptr) UnmapViewOfFile(block);
    if (mapping != nullptr) CloseHandle(mapping);
    if (retry != nullptr) CloseHandle(retry);
    block = nullptr;
    mapping = nullptr;
    retry = nullptr;
}

void OfaEnsureLoaded() {
    if (ofaLoaded) return;
    ofps::feeder::Init(State().module);
    if (!ofps::feeder::Available() || !ofps::feeder::Load(&ofa)) return;
    ofaLoaded = true;
    ::reshade::log::message(::reshade::log::level::info,
        "Optimizer FPS: running inside the DLSS 5 feed host; the optical flow settings are editable in the tab");
}
bool OfaLoaded() { return ofaLoaded; }
ofps::feeder::Settings &OfaSettings() { return ofa; }
void OfaRefresh() {
    if (!ofaLoaded) OfaEnsureLoaded();
    else if (--ofaRefreshIn <= 0) { ofaRefreshIn = 120; ofps::feeder::Refresh(&ofa); }
    if (!DirectHostActive() && Core()) Core()->SetHostMotionGrid(
        ofaLoaded && ofa.source != ofps::feeder::SourceShader ? ofa.grid : 0);
}
void OfaSave() {
    if (DirectHostActive() || !ofaLoaded) return;
    if (ofps::feeder::Save(ofa)) {
        char line[192];
        std::snprintf(line, sizeof(line),
            "Optimizer FPS: optical flow -- mv_source=%s ofa_grid=%d ofa_perf=%d written to %s",
            ofa.source == ofps::feeder::SourceShader ? "shader" : "ofa", ofa.grid, ofa.perf,
            ofps::feeder::CfgPath().c_str());
        ::reshade::log::message(::reshade::log::level::info, line);
    } else {
        ::reshade::log::message(::reshade::log::level::warning,
            "Optimizer FPS: the optical flow settings could not be written to dlss5-feed-host64.cfg");
    }
}

void RemotePublish() {
    EnsureBlock();
    if (block == nullptr) return;
    if (retry != nullptr && WaitForSingleObject(retry, 0) == WAIT_OBJECT_0 && SafeMode()) {
        CrashGuardRetry();
        ::reshade::log::message(::reshade::log::level::info,
            "Optimizer FPS: crash guard - Retry pressed in the game's tab; warping again in this session");
    }
    ApplyEdit();
    ofps::remote::WireSnapshot next{};
    FillRemoteSnapshot(next);
    next.appliedSettingsGeneration = seenSettings;
    ofps::remote::CopyText(next.shell.connection, sizeof(next.shell.connection), editError);
    ofps::remote::PublishHostSnapshot(*block, next);
}

} // namespace ofps::reshade
