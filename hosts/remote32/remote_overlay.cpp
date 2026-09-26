#define NOMINMAX
#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>

#include "remote_overlay.h"
#include "remote_ui_source.h"
#include "remote_link.h"
#include "../reshade/addon/overlay_zone.h"
#include "../common/overlay_widgets.h"
#include "core/api/ofps_ui.inl"
#include "ofps_version.h"

#include <algorithm>
#include <cstdio>

namespace ofps::remote {
namespace {
RemoteUiSource source;

struct Hooks : ui::PendingUiEdits {
    bool padPending = false;
    float padX = 0, padY = 0;
    const char *Label(const OfpsSettingDesc &desc) { return ui::DisplayLabel(desc.label); }
    ui::SliderResult SliderFloatWithReset(const OfpsSettingDesc &desc,
                                          const char *label, float &value,
                                          float lo, float hi) {
        const auto row = ui::SliderWithReset(label, &value, lo, hi,
                                              desc.defaultValue.f, "%.1f", nullptr, true);
        return {row.changed, row.active};
    }
    ui::SliderResult SliderIntWithReset(const OfpsSettingDesc &desc,
                                        const char *label, int &value,
                                        int lo, int hi) {
        const auto row = ui::SliderIntWithReset(label, &value, lo, hi,
                                                 desc.defaultValue.i);
        return {row.changed, row.active};
    }
    void BeforeGroup(std::uint32_t group, const ui::UiSnapshot &snapshot) {
        if (snapshot.readOnly || snapshot.values.v[OFPS_SET_MODE].i != 2)
            padPending = false;
        if (group != OFPS_GROUP_ZONE_POSITION ||
            snapshot.values.v[OFPS_SET_MODE].i != 2 || !snapshot.preview.nativeW) return;
        float x = padPending ? padX : snapshot.values.v[OFPS_SET_OFFSET_X].f;
        float y = padPending ? padY : snapshot.values.v[OFPS_SET_OFFSET_Y].f;
        if (snapshot.readOnly) ImGui::BeginDisabled();
        const bool changed = ofps::reshade::DrawZonePad(x, y, &snapshot.preview);
        const bool active = ImGui::IsItemActive();
        if (!snapshot.readOnly && changed && active) {
            padPending = true;
            padX = x; padY = y;
        } else if (!snapshot.readOnly && (changed || (padPending && !active))) {
            padPending = false;
            OfpsSettingValue value{};
            value.f = x; source.Commit(OFPS_SET_OFFSET_X, value);
            value.f = y; source.Commit(OFPS_SET_OFFSET_Y, value);
        }
        if (ImGui::Button("Center both")) {
            padPending = false;
            OfpsSettingValue value{};
            source.Commit(OFPS_SET_OFFSET_X, value);
            source.Commit(OFPS_SET_OFFSET_Y, value);
        }
        if (snapshot.readOnly) ImGui::EndDisabled();
    }
    void AfterGroup(std::uint32_t, const ui::UiSnapshot &) {}
    void DrawCustom(std::uint32_t, const ui::UiSnapshot &) {}
};
Hooks hooks;

// Status text that changes with the state stays on one line (nothing below it moves); the full text
// is in the tooltip when it is wider than the tab.
void StatusLine(const ImVec4 &colour, const char *text) {
    ImGui::TextColored(colour, "%s", text);
    if (ImGui::IsItemHovered() && ImGui::CalcTextSize(text).x > ImGui::GetContentRegionAvail().x)
        ImGui::SetTooltip("%s", text);
}

bool g_showAdvancedLoaded = false, g_showAdvanced = false;

bool ShowAdvancedValue() {
    if (!g_showAdvancedLoaded) {
        g_showAdvancedLoaded = true;
        int stored = 0;
        if (::reshade::get_config_value(nullptr, "OptimizerFPS", "ShowAdvanced", stored)) g_showAdvanced = stored != 0;
    }
    return g_showAdvanced;
}

void DrawShowAdvanced() {
    bool value = ShowAdvancedValue();
    ImGui::Separator();
    if (ImGui::Checkbox("Advanced (diagnostics)", &value)) {
        g_showAdvanced = value;
        ::reshade::set_config_value(nullptr, "OptimizerFPS", "ShowAdvanced", value ? 1 : 0);
    }
}

void DrawStatus(const ui::UiSnapshot &snapshot) {
    if (snapshot.status.active) {
        ImGui::TextColored(ImVec4(0.35f, 1.0f, 0.45f, 1.0f),
            "Optimizer FPS ACTIVE: model %ux%u of %ux%u",
            snapshot.status.workW, snapshot.status.workH,
            snapshot.status.nativeW, snapshot.status.nativeH);
    } else if (source.Shell().crashGuard) {
        ImGui::TextWrapped("CRASH GUARD: Neural Rendering runs untouched until Retry.");
        if (RetryAvailable() && ImGui::Button("Retry warping now")) RequestRetry();
    } else {
        char line[512]{};
        std::snprintf(line, sizeof(line), "Optimizer FPS NOT ACTIVE: %s",
                      snapshot.status.reason[0] ? snapshot.status.reason : "unknown");
        StatusLine(ImVec4(1.0f, 0.55f, 0.15f, 1.0f), line);
    }
}

void DrawFeeder() {
    const WireShell &shell = source.Shell();
    if (!shell.feederAvailable) return;
    ImGui::SeparatorText("Motion vectors (optical flow in the 64-bit host)");
    int selectedSource = shell.feederSource == 2 ? 1 : 0;
    int selectedGrid = shell.feederGrid == 4 ? 2 : shell.feederGrid == 2 ? 1 : 0;
    int selectedPerf = shell.feederPerf == 5 ? 2 : shell.feederPerf == 10 ? 1 : 0;
    const char *sources[] = {"ofa (driver optical flow)", "shader (ReShade estimate)"};
    const char *grids[] = {"1 = per pixel", "2", "4"};
    const char *qualities[] = {"fast", "medium", "slow"};
    bool changed = false;
    if (ImGui::Combo("Source", &selectedSource, sources, 2)) changed = true;
    if (ImGui::Combo("Grid", &selectedGrid, grids, 3)) changed = true;
    if (ImGui::Combo("Quality", &selectedPerf, qualities, 3)) changed = true;
    if (changed) source.CommitFeeder(selectedSource == 1 ? 2u : 1u,
                               selectedGrid == 2 ? 4u : selectedGrid == 1 ? 2u : 1u,
                               selectedPerf == 2 ? 5u : selectedPerf == 1 ? 10u : 20u);
    ImGui::TextWrapped("Host now: %s, grid %u, quality %u",
        shell.feederSource == 2 ? "shader" : "ofa", shell.feederGrid, shell.feederPerf);
}
} // namespace

void DrawOverlay(bool fitWindow) {
    constexpr float minWidth = 560.0f;
    ImGui::Dummy(ImVec2(minWidth, 0));
    ImGui::TextDisabled("Optimizer FPS for DLSS5 %s - remote tab", OFPS_ADDON_VERSION_STRING);
    ui::UiSnapshot snapshot{};
    if (!source.Snapshot(snapshot)) {
        hooks.ClearAllPending();
        hooks.padPending = false;
        switch (State()) {
        case Connection::OtherVersion:
            ImGui::TextWrapped("host add-on of another version"); break;
        case Connection::Disconnected:
            ImGui::TextWrapped("host disconnected"); break;
        default:
            ImGui::TextWrapped("Optimizer FPS: the 64-bit host in host64 (DLSS5-Feeder or DLSS5-Reshade-AIO) is not running yet"); break;
        }
    } else {
        DrawStatus(snapshot);
        if (source.Shell().connection[0])
            StatusLine(ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled), source.Shell().connection);
        ImGui::Separator();
        hooks.showAdvanced = ShowAdvancedValue();
        ui::DrawSettings(source, hooks, snapshot);
        if (!snapshot.directHost) DrawFeeder();
        DrawShowAdvanced();
    }
    if (fitWindow) {
        const float width = std::max(minWidth + 2 * ImGui::GetStyle().WindowPadding.x,
                                     ImGui::GetWindowWidth());
        ImGui::SetWindowSize(ImVec2(width, ImGui::GetCursorPosY() +
                                          ImGui::GetStyle().WindowPadding.y), ImGuiCond_Always);
    }
}

} // namespace ofps::remote
