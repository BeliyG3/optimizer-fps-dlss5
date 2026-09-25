#define NOMINMAX
#ifdef OFPS_UI_RESHADE
#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>
#else
#include <imgui.h>
#endif

#include "overlay_widgets.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace ofps::ui {

const char *DisplayLabel(const char *label) {
    if (label == nullptr) return "";
    constexpr std::size_t kMaxCharacters = 36;
    if (std::strlen(label) <= kMaxCharacters) return label;
    static thread_local std::string shortened;
    shortened.assign(label, kMaxCharacters - 3);
    shortened += "...";
    return shortened.c_str();
}

float ControlWidthFor(const char *label, bool withIcon)
{
    const ImGuiStyle &style = ImGui::GetStyle();
    const float labelWidth = ImGui::CalcTextSize(label, nullptr, true).x;
    float width = ImGui::GetContentRegionAvail().x - labelWidth - style.ItemInnerSpacing.x;
    if (withIcon) width -= ImGui::GetFrameHeight() + style.ItemSpacing.x;
    return std::max(100.0f, width);
}

bool ResetIconButton(const char *id, const char *tooltip)
{
    const float size = ImGui::GetFrameHeight();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::Button(id, ImVec2(size, size));
    ImDrawList *draw = ImGui::GetWindowDrawList();
    const ImVec2 centre(pos.x + size * 0.5f, pos.y + size * 0.5f);
    const float radius = size * 0.27f;
    const ImU32 colour = ImGui::GetColorU32(ImGuiCol_Text);
    constexpr float kPi = 3.14159265f;
    draw->PathArcTo(centre, radius, 0.55f * kPi, 2.05f * kPi, 24);
    draw->PathStroke(colour, 0, 1.6f);
    const float tipAngle = 2.05f * kPi;
    const ImVec2 tip(centre.x + radius * std::cos(tipAngle), centre.y + radius * std::sin(tipAngle));
    const float head = size * 0.16f;
    draw->AddTriangleFilled(ImVec2(tip.x - head, tip.y - head * 0.2f), ImVec2(tip.x + head * 0.6f, tip.y - head),
                            ImVec2(tip.x + head * 0.4f, tip.y + head * 0.9f), colour);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tooltip);
    return pressed;
}

SliderRow SliderWithReset(const char *label, float *value, float lo, float hi, float defaultValue,
                          const char *format, const char *resetTooltip, bool doubleClickResets, const char *idScope)
{
    ImGui::SetNextItemWidth(ControlWidthFor(label, true));
    ImGui::PushID(idScope != nullptr ? idScope : label);
    const bool edited = ImGui::SliderFloat("##slider", value, lo, hi, format, ImGuiSliderFlags_AlwaysClamp);
    const bool doubleClicked = doubleClickResets && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
    const bool active = ImGui::IsItemActive() && !doubleClicked;
    ImGui::SameLine();
    char tip[128];
    if (resetTooltip == nullptr) {
        // The default is printed the way the slider prints its value.
        char pattern[64];
        std::snprintf(pattern, sizeof(pattern), "Reset to the default (%s)", format);
        std::snprintf(tip, sizeof(tip), pattern, defaultValue);
    }
    const bool reset = ResetIconButton("##reset", resetTooltip != nullptr ? resetTooltip : tip);
    ImGui::SameLine();
    ImGui::TextUnformatted(label);
    ImGui::PopID();
    if (reset || doubleClicked) {
        *value = defaultValue;
        return {true, false};
    }
    return {edited, active};
}

SliderRow SliderIntWithReset(const char *label, int *value, int lo, int hi, int defaultValue)
{
    ImGui::SetNextItemWidth(ControlWidthFor(label, true));
    ImGui::PushID(label);
    const bool edited = ImGui::SliderInt("##slider", value, lo, hi, "%d", ImGuiSliderFlags_AlwaysClamp);
    const bool doubleClicked = ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
    const bool active = ImGui::IsItemActive() && !doubleClicked;
    ImGui::SameLine();
    char tip[128];
    std::snprintf(tip, sizeof(tip), "Reset to the default (%d)", defaultValue);
    const bool reset = ResetIconButton("##reset", tip);
    ImGui::SameLine();
    ImGui::TextUnformatted(label);
    ImGui::PopID();
    if (reset || doubleClicked) {
        *value = defaultValue;
        return {true, false};
    }
    return {edited, active};
}

} // namespace ofps::ui
