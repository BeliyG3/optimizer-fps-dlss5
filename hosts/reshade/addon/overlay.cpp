#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>

#include "overlay.h"

#include "addon_context.h"
#include "config_store.h"
#include "crash_guard.h"
#include "remote_host.h"
#include "../shell_host.h"
#include "../direct_host.h"
#include "../ngx_hook_api.h"
#include "../feeder_ofa_cfg.h"
#include "../../common/overlay_widgets.h"
#include "overlay_schema.h"
#include "ini_store.h"
#include "optiscaler_link.h"
#include "optimizer_fps/types_v2.h"
#include "ofps_version.h"

#include <algorithm>
#include <cstdio>

namespace ofps::reshade {
namespace {

using ofps::ui::ControlWidthFor;

// One line of status whose text changes with the state: never wrapped, so nothing below it moves;
// the full text is in the tooltip when the line is wider than the tab.
void StatusLine(const ImVec4 &colour, const char *text)
{
    ImGui::TextColored(colour, "%s", text);
    if (ImGui::IsItemHovered() && ImGui::CalcTextSize(text).x > ImGui::GetContentRegionAvail().x) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 40.0f);
        ImGui::TextWrapped("%s", text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

bool g_showAdvancedLoaded = false;
bool g_showAdvanced = false;

bool ShowAdvanced()
{
    if (!g_showAdvancedLoaded) {
        g_showAdvancedLoaded = true;
        int value = 0;
        if (::reshade::get_config_value(nullptr, ActiveIniSection(), "ShowAdvanced", value))
            g_showAdvanced = value != 0;
    }
    return g_showAdvanced;
}

void DrawShowAdvanced()
{
    bool value = ShowAdvanced();
    if (ImGui::Checkbox("Advanced (diagnostics)", &value)) {
        g_showAdvanced = value;
        ::reshade::set_config_value(nullptr, ActiveIniSection(), "ShowAdvanced", value ? 1 : 0);
    }
}

// OptiScaler's own NR settings that duplicate this add-on's work.
void DrawOptiScalerDuplicates()
{
    const OptiScalerLink &link = OptiScalerState();
    if (!link.present) return;
    if (link.ownCompression)
        StatusLine(ImVec4(1.0f, 0.35f, 0.3f, 1.0f),
                   "OptiScaler compresses the periphery too (SpatialCompression): the frame is compressed twice. "
                   "Turn it off in OptiScaler's Neural Rendering settings, or set Mode to Off here.");
    if (link.ownPasses > 1)
        StatusLine(ImVec4(1.0f, 0.55f, 0.15f, 1.0f),
                   "OptiScaler runs extra model passes (Passes > 1); use them there or here, not in both.");
}

void DrawStatusBanner(const OfpsStatus &hook, const ofps::sdk::ConfigV2 &config, bool directHostActive)
{
    if (const char *error = CoreLoadError(); *error) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.3f, 1.0f), "%s", error);
        ImGui::PopTextWrapPos();
    }
    if (directHostActive || !Core()) return;
    const auto hookInfo = GetHookStatus();
    if (ofps::reshade::SafeMode()) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(
            ImVec4(1.0f, 0.35f, 0.3f, 1.0f),
            "CRASH GUARD: the previous session of this game ended right after the first frame this add-on "
            "warped (or was killed). Neural rendering runs untouched by this add-on now. Press Retry to warp "
            "again in this session; if the game dies again, the marker returns.");
        ImGui::PopTextWrapPos();
        if (ImGui::Button("Retry warping now")) CrashGuardRetry();
    }
    char line[512]{};
    if (hook.active) {
        std::snprintf(line, sizeof(line), "Optimizer FPS ACTIVE: model %ux%u of %ux%u%s%s", hook.workW,
                      hook.workH, hook.nativeW, hook.nativeH, hook.adopted ? " (model adopted)" : "",
                      hook.temporalMode == 1   ? ", temporal: interpolating"
                      : hook.temporalMode == 2 ? ", temporal: centre every frame"
                      : hook.temporalMode == 3 ? ", temporal: model in the background"
                                               : "");
        StatusLine(ImVec4(0.35f, 1.0f, 0.45f, 1.0f), line);
    } else {
        const char *why = nullptr;
        if (!hookInfo.moduleFound) why = "nvngx_dlssnr.dll is not loaded in this process (no Neural Rendering host)";
        else if (!hookInfo.hooked) why = hook.reason[0] != 0 ? hook.reason : "hook not installed yet";
        else if (!hook.featureCreated) why = "waiting for the host to create feature 18";
        else if (config.mode == ofps::sdk::WarpMode::Off) why = "mode is Off";
        else why = hook.reason[0] != 0 ? hook.reason : "pass-through";
        if (!hookInfo.hooked && hookInfo.moduleFound)
            std::snprintf(line, sizeof(line), "Optimizer FPS NOT ACTIVE: %s (hook attempts: %u, retried every frame)",
                          why, hookInfo.hookAttempts);
        else
            std::snprintf(line, sizeof(line), "Optimizer FPS NOT ACTIVE: %s", why);
        StatusLine(ImVec4(1.0f, 0.55f, 0.15f, 1.0f), line);
    }
}

void DrawOfa()
{
    // Optical flow, only inside the 64-bit feed host: the motion vectors handed to the model are
    // computed there, and the three keys below are the host's own dlss5-feed-host64.cfg. Writing
    // the file is the whole mechanism -- the host re-reads it every ~60 frames and reopens its
    // optical flow session, so a change here lands within a second with no restart.
    OfaEnsureLoaded();
    if (OfaLoaded()) {
        ImGui::SeparatorText("Motion vectors (optical flow in the 64-bit host)");
        bool ofaChanged = false;
        // "Auto" is the first entry so the combo reads like the rest of the tab; it means the same
        // thing the host's default does -- use the driver's engine when it opens.
        const char *sources[] = {"Auto (the driver's engine when it opens)",
                                 "ofa (driver optical flow)",
                                 "shader (ReShade estimate)"};
        int sourceIndex = OfaSettings().source == ofps::feeder::SourceShader ? 2 : 1;
        ImGui::SetNextItemWidth(ControlWidthFor("Source", false));
        if (ImGui::Combo("Source", &sourceIndex, sources, 3)) {
            OfaSettings().source = sourceIndex == 2 ? ofps::feeder::SourceShader : ofps::feeder::SourceOfa;
            ofaChanged = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("ofa: the display driver's optical flow engine computes the vectors from the frame inside the host\n"
                              "(NVIDIA only, and it never reports zero motion the way the shader estimate does below one pixel).\n"
                              "shader: the vectors the 32-bit add-on sends, a ReShade effect's estimate.");
        const char *grids[] = {"1 = per pixel", "2", "4"};
        int gridIndex = OfaSettings().grid == 1 ? 0 : OfaSettings().grid == 4 ? 2 : 1;
        ImGui::SetNextItemWidth(ControlWidthFor("Grid", false));
        if (ImGui::Combo("Grid", &gridIndex, grids, 3)) {
            OfaSettings().grid = gridIndex == 0 ? 1 : gridIndex == 2 ? 4 : 2;
            ofaChanged = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("One flow vector per grid x grid pixels, upsampled to full resolution.\n"
                              "1 is the finest and the most expensive; 4 is cheap and coarse. 2 is the balance.");
        const char *quality[] = {"fast", "medium", "slow"};
        int perfIndex = OfaSettings().perf == 5 ? 2 : OfaSettings().perf == 10 ? 1 : 0;
        ImGui::SetNextItemWidth(ControlWidthFor("Quality", false));
        if (ImGui::Combo("Quality", &perfIndex, quality, 3)) {
            OfaSettings().perf = perfIndex == 2 ? 5 : perfIndex == 1 ? 10 : 20;
            ofaChanged = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("The engine's own performance level: slow searches hardest and costs the most GPU time.");
        if (ofaChanged) OfaSave();
        ImGui::TextDisabled("Saved to the host's dlss5-feed-host64.cfg (?)");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Written to %s. The host re-reads it while it runs and reopens the optical flow session;\n"
                              "one frame after the change falls back to the shader vectors while the engine primes.",
                              ofps::feeder::CfgPath().c_str());
    }
}

void DrawOverlayBody(bool fitWindow)
{
    constexpr float kMinimumContentWidth = 560.0f;
    ImGui::Dummy(ImVec2(kMinimumContentWidth, 0.0f));
    ImGui::TextDisabled("Optimizer FPS for DLSS5 %s", OFPS_ADDON_VERSION_STRING);

    ofps::sdk::Status configStatus = ofps::sdk::Status::Ok;
    const auto config = CurrentConfig(configStatus);
    const bool directHost = DirectHostActive();
    ofps::ui::UiSnapshot snapshot{};
    snapshot.directHost = directHost;
    if (IOfpsCore *core = Core()) {
        OfpsHostCaps caps{};
        caps.size = sizeof(caps);
        ofps::ui::CoreUiSource source(*core, caps, directHost, DirectHostActive,
                                      nullptr, SaveOneSettingToReShadeIni);
        if (source.Snapshot(snapshot)) {
            if (directHost && State().directValuesReady)
                snapshot.values = State().directValues;
            DrawStatusBanner(snapshot.status, config, directHost);
            DrawOptiScalerDuplicates();
            ImGui::Separator();
            DrawSchemaSettings(source, snapshot, ShowAdvanced());
            if (!DirectHostActive()) AdoptCoreValuesForOverlay();
        } else {
            ImGui::TextDisabled("Core settings unavailable");
        }
    } else {
        if (directHost) ImGui::TextUnformatted("applied by OptiScaler");
        ImGui::TextDisabled("Core settings unavailable");
    }
    if (!DirectHostActive()) DrawOfa();
    ImGui::Separator();
    DrawShowAdvanced();
    if (ShowAdvanced()) DrawDiagnostics(snapshot);
    if (!fitWindow) return;
    const float width = std::max(kMinimumContentWidth +
        2.0f * ImGui::GetStyle().WindowPadding.x, ImGui::GetWindowWidth());
    ImGui::SetWindowSize(ImVec2(width, ImGui::GetCursorPosY() +
        ImGui::GetStyle().WindowPadding.y), ImGuiCond_Always);
}
} // namespace

void DrawOverlay(::reshade::api::effect_runtime *) { DrawOverlayBody(true); }
void DrawOverlayEmbedded(::reshade::api::effect_runtime *) { DrawOverlayBody(false); }

} // namespace ofps::reshade
