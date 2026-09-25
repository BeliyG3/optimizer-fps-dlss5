#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>

#include "overlay_temporal.h"

#include <cstdio>

namespace ofps::reshade {
namespace {

void Line(const char *text)
{
    ImGui::TextUnformatted(text);
    if (ImGui::IsItemHovered() && ImGui::CalcTextSize(text).x > ImGui::GetContentRegionAvail().x)
        ImGui::SetTooltip("%s", text);
}

} // namespace

void DrawTemporalStatus(const OfpsStatus &status, std::uint32_t group, bool showAdvanced) {
    char line[512]{};
    if (group == OFPS_GROUP_MODEL_PASSES) {
        if (status.modelPassReason != nullptr && status.modelPassReason[0] != 0)
            std::snprintf(line, sizeof(line), "Model passes running: %u (%s)", status.modelPassesRunning,
                          status.modelPassReason);
        else
            std::snprintf(line, sizeof(line), "Model passes running: %u", status.modelPassesRunning);
        Line(line);
        return;
    }
    if (status.temporalMode == 0) return;
    if (status.temporalReason != nullptr && status.temporalReason[0] != 0)
        std::snprintf(line, sizeof(line), "Temporal mode not applied: %s", status.temporalReason);
    else
        std::snprintf(line, sizeof(line), "Temporal: model frames %llu, interpolated %llu",
                      static_cast<unsigned long long>(status.fullFrames),
                      static_cast<unsigned long long>(status.interpFrames));
    Line(line);
    if (!showAdvanced) return;
    std::snprintf(line, sizeof(line), "Frames shown as plain colour: %llu%s%s",
                  static_cast<unsigned long long>(status.fallbackFrames),
                  status.fallbackFrames > 0 && status.fallbackReason != nullptr ? ", last: " : "",
                  status.fallbackFrames > 0 && status.fallbackReason != nullptr ? status.fallbackReason : "");
    Line(line);
    if (status.modelSamples > 0)
        std::snprintf(line, sizeof(line), "Model GPU time: %.2f ms (n=%u)", status.modelMs, status.modelSamples);
    else
        std::snprintf(line, sizeof(line), "Model GPU time: - (Model GPU timing in Diagnostics)");
    Line(line);
}

} // namespace ofps::reshade
