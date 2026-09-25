#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>

#include "overlay_zone.h"

#include <algorithm>

namespace ofps::reshade {
// The zone pad: a miniature of the frame with the raw Work region (orange) and the 1:1 centre band
// (cyan); dragging inside it moves the band. Returns true when the offset changed.
bool DrawZonePad(float &offsetX, float &offsetY, const OfpsLayoutPreview *layout)
{
    if (!layout || !layout->nativeW || !layout->nativeH) return false;
    const float nativeW = static_cast<float>(layout->nativeW), nativeH = static_cast<float>(layout->nativeH);
    const float limitX = layout->offsetMaxX, limitY = layout->offsetMaxY;

    const float padW = std::max(160.0f, std::min(ImGui::GetContentRegionAvail().x, 360.0f));
    const float padH = padW * nativeH / nativeW;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##zone_pad", ImVec2(padW, padH));
    const bool active = ImGui::IsItemActive();
    bool changed = false;
    if (active && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        const float fx = (mouse.x - origin.x) / padW;
        const float fy = (mouse.y - origin.y) / padH;
        const float newX = std::clamp((fx - 0.5f) * 100.0f, -limitX, limitX);
        const float newY = std::clamp((fy - 0.5f) * 100.0f, -limitY, limitY);
        if (newX != offsetX || newY != offsetY) {
            offsetX = newX;
            offsetY = newY;
            changed = true;
        }
    }

    ImDrawList *draw = ImGui::GetWindowDrawList();
    const ImVec2 end(origin.x + padW, origin.y + padH);
    draw->AddRectFilled(origin, end, IM_COL32(28, 28, 32, 255));
    draw->AddRect(origin, end, IM_COL32(120, 120, 130, 255));
    const auto toPad = [&](float x, float y) { return ImVec2(origin.x + x * padW, origin.y + y * padH); };
    const auto &w = layout->rawWork; const auto &c = layout->center;
    draw->AddRect(toPad(w.x, w.y), toPad(w.x+w.w, w.y+w.h), IM_COL32(255,115,0,255), 0.0f, 0, 1.5f);
    draw->AddRectFilled(toPad(c.x,c.y),toPad(c.x+c.w,c.y+c.h),IM_COL32(0,160,200,40));
    draw->AddRect(toPad(c.x,c.y),toPad(c.x+c.w,c.y+c.h),IM_COL32(0,210,255,255),0.0f,0,1.5f);
    const ImVec2 handle = toPad(0.5f + offsetX * 0.01f,
                                0.5f + offsetY * 0.01f);
    draw->AddCircleFilled(handle, 5.0f, active ? IM_COL32(255, 255, 255, 255) : IM_COL32(0, 210, 255, 255));
    draw->AddLine(ImVec2(origin.x + 0.5f * padW, origin.y), ImVec2(origin.x + 0.5f * padW, end.y), IM_COL32(90, 90, 100, 120));
    draw->AddLine(ImVec2(origin.x, origin.y + 0.5f * padH), ImVec2(end.x, origin.y + 0.5f * padH), IM_COL32(90, 90, 100, 120));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Drag to move the 1:1 zone (cyan); orange = raw Work region.\n"
                                                  "Work size stays the same: the wider side is compressed harder, the narrower one never below 1:1.");
    return changed;
}

} // namespace ofps::reshade
