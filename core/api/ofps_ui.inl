#pragma once

// Include the host's imgui.h before this file. ReShade also includes reshade.hpp.
#include "ofps_ui_source.h"

#include <cstdio>
#include <cstdint>

namespace ofps::ui {

inline const char *GroupName(std::uint32_t group) {
    static const char *const names[OFPS_GROUP_COUNT] = {
        "Mode", "Zone size", "Zone position", "Outlines",
        "Output colour", "Temporal", "Model passes", "Motion source",
        "Diagnostics"
    };
    return group < OFPS_GROUP_COUNT ? names[group] : "Unknown";
}

struct SliderResult { bool changed; bool active; };

inline void ShowHelp(const char *help) {
    if (help == nullptr || !ImGui::IsItemHovered()) return;
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 40.0f);
    ImGui::TextWrapped("%s", help);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

inline bool DrawEnum(const OfpsSettingDesc &desc, const char *label,
                     OfpsSettingValue &value) {
    const EnumChoices choices = ChoicesFor(desc);
    if (choices.count == 0) return false;
    int selected = 0;
    for (int i = 0; i < choices.count; ++i)
        if (choices.values[i] == value.i) selected = i;
    if (desc.id == OFPS_SET_TEMPORAL_MODE && value.i == 2) selected = 1;
    if (!ImGui::Combo(label, &selected, choices.labels.data(), choices.count)) return false;
    value.i = choices.values[selected];
    return true;
}

template <class Source, class Hooks>
bool DrawSetting(Source &source, Hooks &hooks, const OfpsSettingDesc &desc,
                 const UiSnapshot &snapshot) {
    if (desc.id == OFPS_SET_GLOBAL_SCALE &&
        (snapshot.caps.flags & OFPS_CAP_MODEL_RESOLUTION) != 0u) {
        hooks.ClearPending(desc.id);
        return false;
    }
    if (!Visible(desc, snapshot.values)) {
        hooks.ClearPending(desc.id);
        return false;
    }
    const char *label = hooks.Label(desc);
    if (!Supported(desc, snapshot) || !snapshot.available[desc.id]) {
        const char *why = snapshot.unavailableReason[desc.id];
        ImGui::TextWrapped("%s: %s", label,
                           why != nullptr ? why : "unavailable on this host");
        return false;
    }
    if ((desc.flags & OFPS_FLAG_DIAGNOSTIC) != 0u) {
        char shown[64]{};
        if (desc.type == OFPS_TYPE_FLOAT)
            std::snprintf(shown, sizeof(shown), "%.9g",
                          static_cast<double>(snapshot.values.v[desc.id].f));
        else
            std::snprintf(shown, sizeof(shown), "%d", snapshot.values.v[desc.id].i);
        ImGui::TextWrapped("%s: %s", label, shown);
        ShowHelp(desc.help);
        return false;
    }
    if (desc.rangeFrom != 0u && !snapshot.ranges[desc.id].available) {
        ImGui::TextWrapped("%s: range unavailable", label);
        return false;
    }
    const SettingRange &range = snapshot.ranges[desc.id];
    const float lo = desc.rangeFrom != 0u ? range.lo : desc.minValue;
    const float hi = desc.rangeFrom != 0u ? range.hi : desc.maxValue;
    OfpsSettingValue edited = hooks.PendingValue(desc.id, snapshot.values.v[desc.id]);
    ImGui::PushID(static_cast<int>(desc.id));
    if (snapshot.readOnly) ImGui::BeginDisabled();
    bool changed = false;
    bool active = false;
    switch (desc.type) {
    case OFPS_TYPE_BOOL: {
        bool selected = edited.i != 0;
        changed = ImGui::Checkbox(label, &selected);
        if (changed) edited.i = selected ? 1 : 0;
        break;
    }
    case OFPS_TYPE_INT: {
        const SliderResult row = hooks.SliderIntWithReset(
            desc, label, edited.i, static_cast<int>(lo), static_cast<int>(hi));
        changed = row.changed;
        active = row.active;
        break;
    }
    case OFPS_TYPE_FLOAT: {
        const SliderResult row = hooks.SliderFloatWithReset(desc, label, edited.f, lo, hi);
        changed = row.changed;
        active = row.active;
        break;
    }
    case OFPS_TYPE_ENUM: changed = DrawEnum(desc, label, edited); break;
    default: break;
    }
    if (snapshot.readOnly) ImGui::EndDisabled();
    ShowHelp(desc.help);
    ImGui::PopID();
    if (snapshot.readOnly) {
        hooks.ClearPending(desc.id);
        return false;
    }
    if (desc.type == OFPS_TYPE_INT || desc.type == OFPS_TYPE_FLOAT)
        return hooks.CommitOnRelease(source, desc.id, edited, changed, active);
    return changed && source.Commit(desc.id, edited);
}

// A group whose every setting is hidden on this host or in this mode gets no header at all.
inline bool GroupHasContent(std::uint32_t group, const UiSnapshot &snapshot) {
    for (std::uint32_t id = 0; id < OFPS_SET_COUNT; ++id) {
        const OfpsSettingDesc &desc = kOfpsSettings[id];
        if (desc.group != group) continue;
        if (desc.id == OFPS_SET_GLOBAL_SCALE &&
            (snapshot.caps.flags & OFPS_CAP_MODEL_RESOLUTION) != 0u) continue;
        if (desc.customWidget == 0u && !Visible(desc, snapshot.values)) continue;
        return true;
    }
    return false;
}

template <class Source, class Hooks>
void DrawSettings(Source &source, Hooks &hooks, const UiSnapshot &snapshot) {
    if (snapshot.connectionLost) {
        hooks.ClearAllPending();
        ImGui::TextWrapped("host disconnected");
        return;
    }
    if (snapshot.directHost) ImGui::TextWrapped("applied by OptiScaler");
    if (snapshot.readOnly) hooks.ClearAllPending();
    // Layout: Mode on top without a header; the four compression groups under one collapsible
    // "Compression" header, each introduced by a separator; the remaining groups as their own
    // headers; Diagnostics only when the host shows advanced settings.
    bool compressionOpen = false;
    for (std::uint32_t group = 0; group < OFPS_GROUP_COUNT; ++group) {
        if ((group == OFPS_GROUP_DIAGNOSTICS && !hooks.showAdvanced) ||
            (group > OFPS_GROUP_OUTPUT_COLOUR && !GroupHasContent(group, snapshot))) {
            hooks.ClearGroupPending(group);
            continue;
        }
        const bool compressionPart = group >= OFPS_GROUP_ZONE_SIZE && group <= OFPS_GROUP_OUTPUT_COLOUR;
        ImGui::PushID(static_cast<int>(group));
        if (group == OFPS_GROUP_ZONE_SIZE) {
            ImGui::SetNextItemOpen(true, ImGuiCond_Once);
            compressionOpen = ImGui::CollapsingHeader("Compression");
        }
        bool open = true;
        if (compressionPart) {
            open = compressionOpen;
            if (open) ImGui::SeparatorText(GroupName(group));
        } else if (group != OFPS_GROUP_MODE) {
            bool collapsed = false;
            for (std::uint32_t id = 0; id < OFPS_SET_COUNT; ++id)
                if (kOfpsSettings[id].group == group &&
                    (kOfpsSettings[id].flags & OFPS_FLAG_COLLAPSED_GROUP) != 0u)
                    collapsed = true;
            ImGui::SetNextItemOpen(!collapsed, ImGuiCond_Once);
            open = ImGui::CollapsingHeader(GroupName(group));
        }
        if (!open) {
            hooks.ClearGroupPending(group);
            ImGui::PopID();
            continue;
        }
        hooks.BeforeGroup(group, snapshot);
        for (std::uint32_t pass = 0; pass < 2u; ++pass) {
            bool hasAdvanced = false;
            if (pass == 1u) {
                for (std::uint32_t id = 0; id < OFPS_SET_COUNT; ++id)
                    if (kOfpsSettings[id].group == group &&
                        (kOfpsSettings[id].flags & OFPS_FLAG_ADVANCED) != 0u)
                        hasAdvanced = true;
                if (!hasAdvanced || !ImGui::TreeNode("Advanced")) continue;
            }
            for (std::uint32_t id = 0; id < OFPS_SET_COUNT; ++id) {
                const OfpsSettingDesc &desc = kOfpsSettings[id];
                if (desc.group != group) continue;
                if (((desc.flags & OFPS_FLAG_ADVANCED) != 0u) != (pass == 1u)) continue;
                if (desc.customWidget != 0u)
                    hooks.DrawCustom(desc.customWidget, snapshot);
                else
                    DrawSetting(source, hooks, desc, snapshot);
            }
            if (pass == 1u) ImGui::TreePop();
        }
        hooks.AfterGroup(group, snapshot);
        ImGui::PopID();
    }
    if (!hooks.showAdvanced) return;
    OfpsStatusRow rows[32]{};
    const std::uint32_t n = source.StatusLines(rows, 32u);
    for (std::uint32_t i = 0; i < n && i < 32u; ++i)
        if (rows[i].label != nullptr && rows[i].value != nullptr)
            ImGui::TextWrapped("%s: %s", rows[i].label, rows[i].value);
}

template <class Source, class Hooks>
void DrawSettings(Source &source, Hooks &hooks) {
    UiSnapshot snapshot{};
    if (!source.Snapshot(snapshot)) {
        hooks.ClearAllPending();
        ImGui::TextWrapped("Settings are not available yet");
        return;
    }
    DrawSettings(source, hooks, snapshot);
}

} // namespace ofps::ui
