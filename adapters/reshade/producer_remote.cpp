#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>

#include "pw_remote_ipc.h"
#include "pw_version.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

// Optimizer FPS remote overlay (stage 26.11).
//
// A 32-bit ReShade add-on with no renderer of its own. In the DLSS 5 x86 kits the game (Dreamfall,
// The Witcher 2, Dragon Age: Origins, ...) runs ReShade x86 + dlss5-feed.addon32 and the Neural
// Rendering model runs in a child process, host64\dlss5-feed-host64.exe, where ReShade x64 loads
// renodx and optimizer-fps-dlss5.addon64. The x64 add-on's tab is drawn into a window nobody sees, so
// this add-on draws the same tab in the game's own overlay and exchanges settings and status with
// the x64 add-on through the shared block described in pw_remote_ipc.h.

// NAME carries no version: ReShade keys its DisabledAddons list on it (see producer.cpp).
extern "C" __declspec(dllexport) const char *NAME = "Optimizer FPS for DLSS5 (tab for the 64-bit host)";
extern "C" __declspec(dllexport) const char *DESCRIPTION =
    "Optimizer FPS for DLSS5 " PW_ADDON_VERSION_STRING
    " - Shows the Optimizer FPS tab of the 64-bit Neural Rendering host process inside a 32-bit game.";

namespace {

HANDLE g_mapping = nullptr;
void *g_view = nullptr;
// The block as this build understands it, and the version-1 view of the same memory. Exactly one
// of the two is non-null while connected: version 2 moved everything after `settings`, so an older
// host's block can only be read through the frozen layout in the header.
PwRemoteBlockV1 *g_block = nullptr;
PwRemoteBlockLegacyV1 *g_blockLegacy = nullptr;
unsigned int g_blockVersion = 0;
unsigned int g_framesUntilRetry = 0;

// The last status published by the host and whether it is fresh.
PwRemoteStatusV1 g_status{};
PwRemoteSettingsV1 g_applied{};
bool g_hostAlive = false;
unsigned long long g_lastHeartbeat = 0;

// The values shown by the controls: adopted from the host once, then owned by this overlay.
PwRemoteSettingsV1 g_local{};
bool g_localValid = false;

void CloseBlock()
{
    if (g_view != nullptr) UnmapViewOfFile(g_view);
    if (g_mapping != nullptr) CloseHandle(g_mapping);
    g_view = nullptr;
    g_block = nullptr;
    g_blockLegacy = nullptr;
    g_blockVersion = 0;
    g_mapping = nullptr;
    g_localValid = false;
    g_hostAlive = false;
}

// The settings-generation counter, whichever layout the host published.
volatile LONG *SettingsGenerationPtr()
{
    if (g_block != nullptr) return reinterpret_cast<volatile LONG *>(&g_block->settingsGeneration);
    if (g_blockLegacy != nullptr) return reinterpret_cast<volatile LONG *>(&g_blockLegacy->settingsGeneration);
    return nullptr;
}

// The host creates the block; this side only opens it, and retries while it is absent (the host
// process starts after the game and may be restarted between runs).
void EnsureBlock()
{
    if (g_block != nullptr) return;
    if (g_framesUntilRetry != 0) {
        --g_framesUntilRetry;
        return;
    }
    g_framesUntilRetry = 60;
    g_mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, PW_REMOTE_MAPPING_NAME_W);
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
            "Optimizer FPS remote: connected to the Neural Rendering host process (protocol 2)");
    } else if (magic == PW_REMOTE_MAGIC && version == PW_REMOTE_VERSION_LEGACY &&
               size == sizeof(PwRemoteBlockLegacyV1)) {
        // An older host add-on. Everything the two versions share works; the optical-flow group is
        // hidden, because that host has no way to act on it.
        g_blockLegacy = static_cast<PwRemoteBlockLegacyV1 *>(g_view);
        g_blockVersion = PW_REMOTE_VERSION_LEGACY;
        reshade::log::message(reshade::log::level::info,
            "Optimizer FPS remote: connected to the Neural Rendering host process (protocol 1; "
            "the optical flow controls need a newer optimizer-fps-dlss5.addon64)");
    } else {
        reshade::log::message(reshade::log::level::warning,
            "Optimizer FPS remote: the shared block does not match this add-on's version");
        CloseBlock();
        return;
    }
}

// Widens a version-1 status/settings pair into this build's structs. The fields the two versions
// share are laid out identically, so this is a prefix copy with the version-2 tail left at zero.
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

void OnPresent(reshade::api::effect_runtime *)
{
    EnsureBlock();
    if (g_block == nullptr && g_blockLegacy == nullptr) {
        g_hostAlive = false;
        return;
    }
    if (g_block != nullptr) {
        std::memcpy(&g_status, &g_block->status, sizeof(g_status));
        std::memcpy(&g_applied, &g_block->applied, sizeof(g_applied));
    } else {
        AdoptLegacy(*g_blockLegacy);
    }
    g_status.reason[sizeof(g_status.reason) - 1] = 0;
    g_lastHeartbeat = g_block != nullptr ? g_block->hostHeartbeatTick : g_blockLegacy->hostHeartbeatTick;
    const unsigned long long now = GetTickCount64();
    g_hostAlive = g_lastHeartbeat != 0 && now >= g_lastHeartbeat &&
                  (now - g_lastHeartbeat) < kPwRemoteHeartbeatTimeoutMs;
    if (!g_localValid && g_hostAlive) {
        g_local = g_applied;
        g_localValid = true;
    }
}

void PushSettings()
{
    if (g_block != nullptr) {
        std::memcpy(&g_block->settings, &g_local, sizeof(g_local));
    } else if (g_blockLegacy != nullptr) {
        // The shared prefix only; a version-1 host has no field to put the rest in.
        std::memcpy(&g_blockLegacy->settings, &g_local, sizeof(PwRemoteSettingsLegacyV1));
    } else {
        return;
    }
    InterlockedIncrement(SettingsGenerationPtr());
}

float ControlWidthFor(const char *label)
{
    const ImGuiStyle &style = ImGui::GetStyle();
    const float labelWidth = ImGui::CalcTextSize(label, nullptr, true).x;
    return std::max(100.0f, ImGui::GetContentRegionAvail().x - labelWidth - style.ItemInnerSpacing.x);
}

bool g_fitWindow = true;

void DrawOverlay(reshade::api::effect_runtime *)
{
    constexpr float kMinimumContentWidth = 560.0f;
    ImGui::Dummy(ImVec2(kMinimumContentWidth, 0.0f));
    ImGui::TextDisabled("Optimizer FPS for DLSS5 %s - remote tab", PW_ADDON_VERSION_STRING);

    if ((g_block == nullptr && g_blockLegacy == nullptr) || !g_hostAlive) {
        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                           "Optimizer FPS: host process not running (no shared block)");
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("The model runs in host64\\dlss5-feed-host64.exe. This tab connects to the "
                            "Optimizer FPS add-on there; it appears as soon as that process presents a frame.");
        ImGui::PopTextWrapPos();
    } else if (g_status.active != 0) {
        ImGui::TextColored(ImVec4(0.35f, 1.0f, 0.45f, 1.0f),
                           "Optimizer FPS ACTIVE: model %ux%u of %ux%u, %llu frames%s",
                           g_status.modelW, g_status.modelH, g_status.nativeW, g_status.nativeH,
                           static_cast<unsigned long long>(g_status.fullFrames + g_status.interpFrames),
                           g_status.temporalMode == 1 ? ", temporal: interpolating"
                               : g_status.temporalMode == 3 ? ", temporal: model in the background" : "");
    } else {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.15f, 1.0f), "Optimizer FPS NOT ACTIVE: %s",
                           g_status.reason[0] != 0 ? g_status.reason : "unknown");
        ImGui::PopTextWrapPos();
    }
    ImGui::Separator();
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("Compression for this game is done by the x86 kit (feed32/host64); Mode here is the "
                        "add-on's own warp - keep Off unless you know why.");
    ImGui::PopTextWrapPos();
    ImGui::Separator();

    if (!g_localValid) {
        ImGui::TextDisabled("Waiting for the host's current settings...");
        const float pendingWidth = std::max(kMinimumContentWidth + 2.0f * ImGui::GetStyle().WindowPadding.x,
                                            ImGui::GetWindowWidth());
        if (g_fitWindow)
            ImGui::SetWindowSize(ImVec2(pendingWidth, ImGui::GetCursorPosY() + ImGui::GetStyle().WindowPadding.y),
                                 ImGuiCond_Always);
        return;
    }

    bool changed = false;
    bool dragging = false;
    const auto slider = [&](const char *label, float *value, float lo, float hi, const char *format = "%.1f") {
        ImGui::SetNextItemWidth(ControlWidthFor(label));
        changed |= ImGui::SliderFloat(label, value, lo, hi, format, ImGuiSliderFlags_AlwaysClamp);
        if (ImGui::IsItemActive()) dragging = true;
    };
    const auto sliderInt = [&](const char *label, std::int32_t *value, int lo, int hi) {
        int shown = static_cast<int>(*value);
        ImGui::SetNextItemWidth(ControlWidthFor(label));
        if (ImGui::SliderInt(label, &shown, lo, hi)) {
            *value = static_cast<std::int32_t>(shown);
            changed = true;
        }
        if (ImGui::IsItemActive()) dragging = true;
    };

    int mode = static_cast<int>(g_local.mode);
    const char *modes[] = {"Off", "Uniform", "Peripheral"};
    ImGui::SetNextItemWidth(ControlWidthFor("Mode"));
    if (ImGui::Combo("Mode", &mode, modes, 3)) { g_local.mode = mode; changed = true; }
    int colorFilter = static_cast<int>(g_local.colorFilter);
    const char *colorFilters[] = {"Bilinear", "Auto (soft: wide pre-filter, cubic unpack)"};
    ImGui::SetNextItemWidth(ControlWidthFor("Color filter"));
    if (ImGui::Combo("Color filter", &colorFilter, colorFilters, 2)) {
        g_local.colorFilter = colorFilter;
        changed = true;
    }
    if (mode != 0) {
        ImGui::SeparatorText("Zone size");
        if (mode == 2) {
            slider("Center X (%)", &g_local.centerX, 1.0f, 99.0f);
            slider("Center Y (%)", &g_local.centerY, 1.0f, 99.0f);
        }
        slider("Work X (%)", &g_local.workX, 25.0f, 100.0f);
        slider("Work Y (%)", &g_local.workY, 25.0f, 100.0f);
    }
    slider("Global scale (%)", &g_local.globalScale, 25.0f, 100.0f);
    if (mode == 2) {
        ImGui::SeparatorText("Zone position");
        slider("Offset X (%)", &g_local.offsetX, -49.0f, 49.0f);
        slider("Offset Y (%)", &g_local.offsetY, -49.0f, 49.0f);
        bool workShift = g_local.workShiftEnabled != 0;
        if (ImGui::Checkbox("Shift the compressed region (outer contour)", &workShift)) {
            g_local.workShiftEnabled = workShift ? 1 : 0;
            if (!workShift) { g_local.workShiftX = 0.0f; g_local.workShiftY = 0.0f; }
            changed = true;
        }
        if (workShift) {
            slider("Work shift X (%)", &g_local.workShiftX, -49.0f, 49.0f);
            slider("Work shift Y (%)", &g_local.workShiftY, -49.0f, 49.0f);
        }
        ImGui::SeparatorText("Outlines");
        bool centerOutline = g_local.showCenterOutline != 0;
        bool workOutline = g_local.showWorkOutline != 0;
        if (ImGui::Checkbox("Show uncompressed center (cyan)", &centerOutline)) {
            g_local.showCenterOutline = centerOutline ? 1 : 0;
            changed = true;
        }
        ImGui::SameLine();
        if (ImGui::Checkbox("Show Work boundary (orange)", &workOutline)) {
            g_local.showWorkOutline = workOutline ? 1 : 0;
            changed = true;
        }
    } else if (mode == 1) {
        ImGui::TextDisabled("Zone position and outlines are available in Peripheral mode only.");
    }
    if (mode != 0) {
        ImGui::SeparatorText("Output colour (warped frame only)");
        slider("Brightness (%)", &g_local.brightness, -20.0f, 20.0f);
        slider("Gamma", &g_local.gamma, 0.7f, 1.4f, "%.3f");
    }

    ImGui::SeparatorText("Temporal (Neural Rendering cadence)");
    const char *temporalModes[] = {"Every frame", "Interpolate: full NR every N-th frame (sync)",
                                   "Interpolate: model in the background (async)"};
    int temporalIndex = g_local.temporalMode == 3 ? 2 : std::clamp(static_cast<int>(g_local.temporalMode), 0, 1);
    ImGui::SetNextItemWidth(ControlWidthFor("Temporal mode"));
    if (ImGui::Combo("Temporal mode", &temporalIndex, temporalModes, 3)) {
        g_local.temporalMode = temporalIndex == 2 ? 3 : temporalIndex;
        changed = true;
    }
    if (g_local.temporalMode != 0) {
        const int lowest = g_local.temporalMode == 3 ? 1 : 2;
        if (g_local.temporalEvery < lowest) { g_local.temporalEvery = lowest; changed = true; }
        sliderInt("Full pass every N frames", &g_local.temporalEvery, lowest, 8);
        sliderInt("GPU frames queued ahead (0 = off)", &g_local.temporalMaxQueue, 0, 8);
    }
    if (g_status.temporalMode != 0 || g_local.temporalMode != 0)
        ImGui::Text("Host: temporal mode %u, model frames %llu, interpolated %llu",
                    g_status.temporalMode, static_cast<unsigned long long>(g_status.fullFrames),
                    static_cast<unsigned long long>(g_status.interpFrames));
    if (g_status.modelMs > 0.0f) ImGui::Text("Model GPU time: %.2f ms", g_status.modelMs);

    // Sliders emit a value every frame while dragged; the host rebuilds its model on every accepted
    // change, so the pending value is sent when the control is released (as the x64 tab does).
    static bool s_pending = false;
    if (changed && dragging) {
        s_pending = true;
    } else if (changed) {
        s_pending = false;
        PushSettings();
    } else if (s_pending && !dragging) {
        s_pending = false;
        PushSettings();
    }

    // Optical flow in the 64-bit host. Shown only when the host speaks protocol 2 AND reports that
    // it found its own dlss5-feed-host64.cfg -- otherwise there is nothing on the far side to write.
    if (g_blockVersion >= PW_REMOTE_VERSION && g_status.ofaAvailable != 0) {
        ImGui::SeparatorText("Motion vectors (optical flow in the 64-bit host)");
        bool ofaChanged = false;
        const char *sources[] = {"Auto (the driver's engine when it opens)",
                                 "ofa (driver optical flow)",
                                 "shader (ReShade estimate)"};
        int sourceIndex = g_local.ofaSource == PwRemoteOfaShader ? 2 : 1;
        ImGui::SetNextItemWidth(ControlWidthFor("Source"));
        if (ImGui::Combo("Source", &sourceIndex, sources, 3)) {
            g_local.ofaSource = sourceIndex == 2 ? PwRemoteOfaShader : PwRemoteOfaOfa;
            ofaChanged = true;
        }
        const char *grids[] = {"1 = per pixel", "2", "4"};
        int gridIndex = g_local.ofaGrid == 1 ? 0 : g_local.ofaGrid == 4 ? 2 : 1;
        ImGui::SetNextItemWidth(ControlWidthFor("Grid"));
        if (ImGui::Combo("Grid", &gridIndex, grids, 3)) {
            g_local.ofaGrid = gridIndex == 0 ? 1 : gridIndex == 2 ? 4 : 2;
            ofaChanged = true;
        }
        const char *quality[] = {"fast", "medium", "slow"};
        int perfIndex = g_local.ofaPerf == 5 ? 2 : g_local.ofaPerf == 10 ? 1 : 0;
        ImGui::SetNextItemWidth(ControlWidthFor("Quality"));
        if (ImGui::Combo("Quality", &perfIndex, quality, 3)) {
            g_local.ofaPerf = perfIndex == 2 ? 5 : perfIndex == 1 ? 10 : 20;
            ofaChanged = true;
        }
        ImGui::TextDisabled("Host now: %s, grid %d, %s. The host re-reads its config while it runs, so this "
                            "takes effect within about a second and needs no restart.",
                            g_status.ofaActive == 1 ? "ofa" : g_status.ofaActive == 0 ? "shader" : "unknown",
                            g_status.ofaGrid,
                            g_status.ofaPerf == 5 ? "slow" : g_status.ofaPerf == 10 ? "medium" : "fast");
        if (ofaChanged) PushSettings();
    }

    if (ImGui::Button("Reload the host's values")) {
        g_local = g_applied;
        s_pending = false;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Host heartbeat: %s", g_hostAlive ? "alive" : "stale");

    const float width = std::max(kMinimumContentWidth + 2.0f * ImGui::GetStyle().WindowPadding.x,
                                 ImGui::GetWindowWidth());
    if (g_fitWindow)
        ImGui::SetWindowSize(ImVec2(width, ImGui::GetCursorPosY() + ImGui::GetStyle().WindowPadding.y),
                             ImGuiCond_Always);
}

// 26.13: drawn inside ReShade's Add-ons tab by default (null title); [PeripheralWarp] FloatingWindow=1 adds the window.
void DrawOverlayEmbedded(reshade::api::effect_runtime *runtime) { g_fitWindow = false; DrawOverlay(runtime); }
void DrawOverlayWindow(reshade::api::effect_runtime *runtime) { g_fitWindow = true; DrawOverlay(runtime); }
bool g_floatingWindow = false;

} // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        if (!reshade::register_addon(module)) return FALSE;
        reshade::register_event<reshade::addon_event::reshade_present>(OnPresent);
        reshade::register_overlay(nullptr, DrawOverlayEmbedded);
        {
            int floating = 0;
            if (reshade::get_config_value(nullptr, "PeripheralWarp", "FloatingWindow", floating)) g_floatingWindow = floating != 0;
        }
        if (g_floatingWindow) reshade::register_overlay("Optimizer FPS for DLSS5", DrawOverlayWindow);
    } else if (reason == DLL_PROCESS_DETACH) {
        if (g_floatingWindow) reshade::unregister_overlay("Optimizer FPS for DLSS5", DrawOverlayWindow);
        reshade::unregister_overlay(nullptr, DrawOverlayEmbedded);
        reshade::unregister_event<reshade::addon_event::reshade_present>(OnPresent);
        reshade::unregister_addon(module);
        CloseBlock();
    }
    return TRUE;
}
