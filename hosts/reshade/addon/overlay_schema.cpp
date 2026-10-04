#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>

#include "overlay_schema.h"
#include "overlay_zone.h"
#include "overlay_temporal.h"
#include "../../common/overlay_widgets.h"
#include "../menu_pipeline.h"
#include "core/api/ofps_ui.inl"

namespace ofps::reshade {
namespace {

// Menu mode's one-line status under its checkbox: never wrapped, the full text in the tooltip when it is wider than the tab.
void DrawMenuStatus() {
    const MenuStatusView view = MenuStatusNow();
    const ImVec4 colour = view.status == MenuStatus::Active        ? ImVec4(0.35f, 1.0f, 0.45f, 1.0f)
                          : view.status == MenuStatus::Unavailable ? ImVec4(1.0f, 0.55f, 0.15f, 1.0f)
                                                                   : ImGui::GetStyle().Colors[ImGuiCol_TextDisabled];
    ImGui::TextColored(colour, "%s", view.line.c_str());
    if (ImGui::IsItemHovered() && ImGui::CalcTextSize(view.line.c_str()).x > ImGui::GetContentRegionAvail().x)
        ImGui::SetTooltip("%s", view.line.c_str());
}

struct OverlayHooks : ofps::ui::PendingUiEdits {
    ofps::ui::CoreUiSource *source = nullptr;
    bool padPending = false;
    float padX = 0.0f, padY = 0.0f;

    const char *Label(const OfpsSettingDesc &desc) {
        return ofps::ui::DisplayLabel(desc.label);
    }

    ofps::ui::SliderResult SliderFloatWithReset(const OfpsSettingDesc &desc,
                                                const char *label, float &value,
                                                float lo, float hi) {
        const auto row = ofps::ui::SliderWithReset(label, &value, lo, hi,
                                                    desc.defaultValue.f, "%.1f", nullptr, true);
        return {row.changed, row.active};
    }

    ofps::ui::SliderResult SliderIntWithReset(const OfpsSettingDesc &desc,
                                              const char *label, int &value,
                                              int lo, int hi) {
        const auto row = ofps::ui::SliderIntWithReset(label, &value, lo, hi,
                                                       desc.defaultValue.i);
        return {row.changed, row.active};
    }

    void BeforeGroup(std::uint32_t group, const ofps::ui::UiSnapshot &snapshot) {
        if (group != OFPS_GROUP_ZONE_POSITION || !source) return;
        if (snapshot.values.v[OFPS_SET_MODE].i != 2 ||
            !snapshot.preview.nativeW || !snapshot.preview.nativeH) {
            padPending = false;
            return;
        }
        if (snapshot.readOnly) {
            padPending = false;
            ImGui::BeginDisabled();
        }
        float x = padPending ? padX : snapshot.values.v[OFPS_SET_OFFSET_X].f;
        float y = padPending ? padY : snapshot.values.v[OFPS_SET_OFFSET_Y].f;
        const bool changed = DrawZonePad(x, y, &snapshot.preview);
        const bool active = ImGui::IsItemActive();
        if (snapshot.readOnly) ImGui::EndDisabled();
        if (!snapshot.readOnly && changed && active) {
            padPending = true;
            padX = x;
            padY = y;
        } else if (!snapshot.readOnly && changed) {
            CommitPad(x, y, snapshot);
        } else if (!snapshot.readOnly && padPending && !active) {
            padPending = false;
            CommitPad(padX, padY, snapshot);
        }
        if (snapshot.readOnly) ImGui::BeginDisabled();
        if (ImGui::Button("Center both") && !snapshot.readOnly) {
            padPending = false;
            CommitPad(0.0f, 0.0f, snapshot);
        }
        if (snapshot.readOnly) ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled("Range X +-%.1f%%, Y +-%.1f%%",
                            snapshot.preview.offsetMaxX, snapshot.preview.offsetMaxY);
    }

    void CommitPad(float x, float y, const ofps::ui::UiSnapshot &snapshot) {
        if (!source) return;
        OfpsSettingValue value{};
        if (x != snapshot.values.v[OFPS_SET_OFFSET_X].f) {
            value.f = x;
            source->Commit(OFPS_SET_OFFSET_X, value);
        }
        if (y != snapshot.values.v[OFPS_SET_OFFSET_Y].f) {
            value.f = y;
            source->Commit(OFPS_SET_OFFSET_Y, value);
        }
    }

    void AfterGroup(std::uint32_t group, const ofps::ui::UiSnapshot &snapshot) {
        if (group == OFPS_GROUP_MODE) DrawMenuStatus();
        if (group == OFPS_GROUP_TEMPORAL || group == OFPS_GROUP_MODEL_PASSES)
            DrawTemporalStatus(snapshot.status, group, showAdvanced);
    }
    void DrawCustom(std::uint32_t, const ofps::ui::UiSnapshot &) {}
};

} // namespace

void DrawSchemaSettings(ofps::ui::CoreUiSource &source,
                        const ofps::ui::UiSnapshot &snapshot, bool showAdvanced) {
    static OverlayHooks hooks;
    hooks.source = &source;
    hooks.showAdvanced = showAdvanced;
    ofps::ui::DrawSettings(source, hooks, snapshot);
    hooks.source = nullptr;
}

} // namespace ofps::reshade
