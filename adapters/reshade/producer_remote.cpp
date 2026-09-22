#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>

#include "pw_remote_ipc.h"
#include "remote/remote_link.h"
#include "remote/overlay_event.h"
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

// One line of work per frame: refresh what the host published and keep the heartbeat.
void OnPresent(reshade::api::effect_runtime *)
{
    pw_remote::Poll();
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
    const PwRemoteStatusV1 &status = pw_remote::Status();
    PwRemoteSettingsV1 &local = pw_remote::Local();
    ImGui::Dummy(ImVec2(kMinimumContentWidth, 0.0f));
    ImGui::TextDisabled("Optimizer FPS for DLSS5 %s - remote tab", PW_ADDON_VERSION_STRING);

    if (!pw_remote::Connected() || !pw_remote::HostAlive()) {
        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                           "Optimizer FPS: host process not running (no shared block)");
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("The model runs in the 64-bit host in host64\\ (DLSS5-Feeder's dlss5-feed-host64.exe or "
                            "DLSS5-Reshade-AIO's 32-bit wrapper). This tab connects to the Optimizer FPS add-on "
                            "there; it appears as soon as that process presents a frame.");
        ImGui::PopTextWrapPos();
    } else if (status.active != 0) {
        ImGui::TextColored(ImVec4(0.35f, 1.0f, 0.45f, 1.0f),
                           "Optimizer FPS ACTIVE: model %ux%u of %ux%u, %llu frames%s",
                           status.modelW, status.modelH, status.nativeW, status.nativeH,
                           static_cast<unsigned long long>(status.fullFrames + status.interpFrames),
                           status.temporalMode == 1 ? ", temporal: interpolating"
                               : status.temporalMode == 3 ? ", temporal: model in the background" : "");
    } else {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.15f, 1.0f), "Optimizer FPS NOT ACTIVE: %s",
                           status.reason[0] != 0 ? status.reason : "unknown");
        ImGui::PopTextWrapPos();
    }
    ImGui::Separator();
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("Compression for this game is done by the x86 kit (feed32/host64); Mode here is the "
                        "add-on's own warp - keep Off unless you know why.");
    ImGui::PopTextWrapPos();
    ImGui::Separator();

    if (!pw_remote::LocalValid()) {
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

    int mode = static_cast<int>(local.mode);
    const char *modes[] = {"Off", "Uniform", "Peripheral"};
    ImGui::SetNextItemWidth(ControlWidthFor("Mode"));
    if (ImGui::Combo("Mode", &mode, modes, 3)) { local.mode = mode; changed = true; }
    int colorFilter = static_cast<int>(local.colorFilter);
    const char *colorFilters[] = {"Bilinear", "Auto (soft: wide pre-filter, cubic unpack)"};
    ImGui::SetNextItemWidth(ControlWidthFor("Color filter"));
    if (ImGui::Combo("Color filter", &colorFilter, colorFilters, 2)) {
        local.colorFilter = colorFilter;
        changed = true;
    }
    if (mode != 0) {
        ImGui::SeparatorText("Zone size");
        if (mode == 2) {
            slider("Center X (%)", &local.centerX, 1.0f, 99.0f);
            slider("Center Y (%)", &local.centerY, 1.0f, 99.0f);
        }
        slider("Work X (%)", &local.workX, 25.0f, 100.0f);
        slider("Work Y (%)", &local.workY, 25.0f, 100.0f);
    }
    slider("Global scale (%)", &local.globalScale, 25.0f, 100.0f);
    if (mode == 2) {
        ImGui::SeparatorText("Zone position");
        slider("Offset X (%)", &local.offsetX, -49.0f, 49.0f);
        slider("Offset Y (%)", &local.offsetY, -49.0f, 49.0f);
        bool workShift = local.workShiftEnabled != 0;
        if (ImGui::Checkbox("Shift the compressed region (outer contour)", &workShift)) {
            local.workShiftEnabled = workShift ? 1 : 0;
            if (!workShift) { local.workShiftX = 0.0f; local.workShiftY = 0.0f; }
            changed = true;
        }
        if (workShift) {
            slider("Work shift X (%)", &local.workShiftX, -49.0f, 49.0f);
            slider("Work shift Y (%)", &local.workShiftY, -49.0f, 49.0f);
        }
        ImGui::SeparatorText("Outlines");
        bool centerOutline = local.showCenterOutline != 0;
        bool workOutline = local.showWorkOutline != 0;
        if (ImGui::Checkbox("Show uncompressed center (cyan)", &centerOutline)) {
            local.showCenterOutline = centerOutline ? 1 : 0;
            changed = true;
        }
        ImGui::SameLine();
        if (ImGui::Checkbox("Show Work boundary (orange)", &workOutline)) {
            local.showWorkOutline = workOutline ? 1 : 0;
            changed = true;
        }
    } else if (mode == 1) {
        ImGui::TextDisabled("Zone position and outlines are available in Peripheral mode only.");
    }
    if (mode != 0) {
        ImGui::SeparatorText("Output colour (warped frame only)");
        slider("Brightness (%)", &local.brightness, -20.0f, 20.0f);
        slider("Gamma", &local.gamma, 0.7f, 1.4f, "%.3f");
    }

    ImGui::SeparatorText("Temporal (Neural Rendering cadence)");
    // Model passes live in the host, so the controls need a host that speaks protocol 3.
    const bool passesSupported = pw_remote::Version() >= PW_REMOTE_VERSION;
    if (passesSupported) {
        if (local.modelPasses < 1) local.modelPasses = 1;
        sliderInt("Model passes", &local.modelPasses, 1, 3);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Each extra pass runs the whole model again on its previous output: sharper, but the "
                              "cost\nis another full pass and each one darkens the picture by about 1%%. A second "
                              "model needs\nabout 1 GB of VRAM in the host process.");
        bool spread = local.spreadPasses == PwRemoteFlagOn;
        if (ImGui::Checkbox("Spread passes over frames", &spread)) {
            local.spreadPasses = spread ? PwRemoteFlagOn : PwRemoteFlagOff;
            changed = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("With a temporal mode and 2-3 passes: one model pass per frame. Only the finished "
                              "cycle is\nshown, the previous one is carried until then, and N is raised to the "
                              "number of passes.");
        ImGui::Text("Model passes running: %d / %d", static_cast<int>(status.modelPassesRunning),
                    static_cast<int>(status.modelPasses));
        if (status.modelPassReason[0] != 0) {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.15f, 1.0f), "%s", status.modelPassReason);
            ImGui::PopTextWrapPos();
        }
    } else {
        ImGui::TextDisabled("Model passes: the host add-on is older than this tab (protocol %u); update "
                            "optimizer-fps-dlss5.addon64 in host64.", pw_remote::Version());
    }
    const char *temporalModes[] = {"Every frame", "Interpolate: full NR every N-th frame (sync)",
                                   "Interpolate: model in the background (async)"};
    int temporalIndex = local.temporalMode == 3 ? 2 : std::clamp(static_cast<int>(local.temporalMode), 0, 1);
    ImGui::SetNextItemWidth(ControlWidthFor("Temporal mode"));
    if (ImGui::Combo("Temporal mode", &temporalIndex, temporalModes, 3)) {
        local.temporalMode = temporalIndex == 2 ? 3 : temporalIndex;
        changed = true;
    }
    if (local.temporalMode != 0) {
        const int lowest = local.temporalMode == 3 ? 1 : 2;
        if (local.temporalEvery < lowest) { local.temporalEvery = lowest; changed = true; }
        sliderInt("Full pass every N frames", &local.temporalEvery, lowest, 8);
        sliderInt("GPU frames queued ahead (0 = off)", &local.temporalMaxQueue, 0, 8);
        // A spread cycle takes one frame per pass, so N can never be shorter than the cycle.
        if (passesSupported && local.spreadPasses == PwRemoteFlagOn && local.modelPasses > 1 &&
            local.temporalEvery < local.modelPasses) {
            local.temporalEvery = local.modelPasses;
            changed = true;
        }
    }
    if (status.temporalMode != 0 || local.temporalMode != 0)
        ImGui::Text("Host: temporal mode %u, model frames %llu, interpolated %llu",
                    status.temporalMode, static_cast<unsigned long long>(status.fullFrames),
                    static_cast<unsigned long long>(status.interpFrames));
    // Why the host is not running the mode that was asked for (no host queue for the background
    // mode, a pass that could not be built, ...). Protocol 3 carries the same text the x64 tab shows.
    if (passesSupported && status.temporalReason[0] != 0) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.15f, 1.0f), "Temporal mode not applied: %s", status.temporalReason);
        ImGui::PopTextWrapPos();
    }
    if (status.modelMs > 0.0f) ImGui::Text("Model GPU time: %.2f ms", status.modelMs);

    // Sliders emit a value every frame while dragged; the host rebuilds its model on every accepted
    // change, so the pending value is sent when the control is released (as the x64 tab does).
    static bool s_pending = false;
    if (changed && dragging) {
        s_pending = true;
    } else if (changed) {
        s_pending = false;
        pw_remote::Push();
    } else if (s_pending && !dragging) {
        s_pending = false;
        pw_remote::Push();
    }

    // Optical flow in the 64-bit host. Shown only when the host speaks protocol 2 or newer AND
    // reports that it found its own dlss5-feed-host64.cfg -- otherwise there is nothing to write.
    if (pw_remote::Version() >= PW_REMOTE_VERSION_OFA && status.ofaAvailable != 0) {
        ImGui::SeparatorText("Motion vectors (optical flow in the 64-bit host)");
        bool ofaChanged = false;
        const char *sources[] = {"Auto (the driver's engine when it opens)",
                                 "ofa (driver optical flow)",
                                 "shader (ReShade estimate)"};
        int sourceIndex = local.ofaSource == PwRemoteOfaShader ? 2 : 1;
        ImGui::SetNextItemWidth(ControlWidthFor("Source"));
        if (ImGui::Combo("Source", &sourceIndex, sources, 3)) {
            local.ofaSource = sourceIndex == 2 ? PwRemoteOfaShader : PwRemoteOfaOfa;
            ofaChanged = true;
        }
        const char *grids[] = {"1 = per pixel", "2", "4"};
        int gridIndex = local.ofaGrid == 1 ? 0 : local.ofaGrid == 4 ? 2 : 1;
        ImGui::SetNextItemWidth(ControlWidthFor("Grid"));
        if (ImGui::Combo("Grid", &gridIndex, grids, 3)) {
            local.ofaGrid = gridIndex == 0 ? 1 : gridIndex == 2 ? 4 : 2;
            ofaChanged = true;
        }
        const char *quality[] = {"fast", "medium", "slow"};
        int perfIndex = local.ofaPerf == 5 ? 2 : local.ofaPerf == 10 ? 1 : 0;
        ImGui::SetNextItemWidth(ControlWidthFor("Quality"));
        if (ImGui::Combo("Quality", &perfIndex, quality, 3)) {
            local.ofaPerf = perfIndex == 2 ? 5 : perfIndex == 1 ? 10 : 20;
            ofaChanged = true;
        }
        ImGui::TextDisabled("Host now: %s, grid %d, %s. The host re-reads its config while it runs, so this "
                            "takes effect within about a second and needs no restart.",
                            status.ofaActive == 1 ? "ofa" : status.ofaActive == 0 ? "shader" : "unknown",
                            status.ofaGrid,
                            status.ofaPerf == 5 ? "slow" : status.ofaPerf == 10 ? "medium" : "fast");
        if (ofaChanged) pw_remote::Push();
    }

    if (ImGui::Button("Reload the host's values")) {
        pw_remote::ReloadFromHost();
        s_pending = false;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Host heartbeat: %s", pw_remote::HostAlive() ? "alive" : "stale");

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
        // The frame-generation presenter hides itself while an overlay is open; in a 32-bit game
        // this tab is the only part of the mod inside the game's process, so it publishes that.
        pw_remote::OverlayEventRegister();
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
        pw_remote::OverlayEventUnregister();
        reshade::unregister_addon(module);
        pw_remote::Close();
    }
    return TRUE;
}
