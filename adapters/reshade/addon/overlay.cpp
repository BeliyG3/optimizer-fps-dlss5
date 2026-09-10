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
#include "layout_bridge.h"
#include "remote_host.h"
#include "../ngx_hook.h"
#include "../pw_ofa_cfg.h"
#include "peripheral_warp/types_v2.h"
#include "pw_version.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace pw_addon {
namespace {

// Native-pixel rectangles of the 1:1 band and of the raw Work region for one axis, from the
// per-side rule of the SDK (sides_v2.h): half the periphery budget per side, never more than the
// side's native pixels, the rest to the wider side.
void ZoneAxisRectangles(float nativeExtent, float rawWorkExtent, float centerFraction, float offsetFraction,
                        float shiftFraction, float *centerLo, float *centerHi, float *workLo, float *workHi)
{
    const float halfBand = 0.5f * centerFraction * nativeExtent;
    const float bandCenter = 0.5f * nativeExtent + offsetFraction * nativeExtent;
    const float halfSpan[2] = {bandCenter, nativeExtent - bandCenter};
    const float periphery[2] = {std::max(0.0f, halfSpan[0] - halfBand), std::max(0.0f, halfSpan[1] - halfBand)};
    const float budget = std::max(0.0f, rawWorkExtent - centerFraction * nativeExtent);
    const int narrow = periphery[0] <= periphery[1] ? 0 : 1;
    float allotted[2] = {};
    allotted[narrow] = std::min(0.5f * budget, periphery[narrow]);
    allotted[1 - narrow] = std::min(budget - allotted[narrow], periphery[1 - narrow]);
    const float maxShift = std::max(0.0f, std::min(allotted[0], periphery[1] - allotted[1]));
    const float minShift = -std::max(0.0f, std::min(allotted[1], periphery[0] - allotted[0]));
    const float shift = std::clamp(shiftFraction * nativeExtent, minShift, maxShift);
    allotted[0] = std::clamp(allotted[0] - shift, 0.0f, periphery[0]);
    allotted[1] = std::clamp(allotted[1] + shift, 0.0f, periphery[1]);
    *centerLo = bandCenter - halfBand;
    *centerHi = bandCenter + halfBand;
    *workLo = bandCenter - halfBand - allotted[0];
    *workHi = bandCenter + halfBand + allotted[1];
}

// Width for a control whose label ImGui draws to its right: the remaining line minus the label's
// real width (and a reset icon when one follows), so labels and icons never fall off the window.
float ControlWidthFor(const char *label, bool withIcon)
{
    const ImGuiStyle &style = ImGui::GetStyle();
    const float labelWidth = ImGui::CalcTextSize(label, nullptr, true).x;
    float width = ImGui::GetContentRegionAvail().x - labelWidth - style.ItemInnerSpacing.x;
    if (withIcon) width -= ImGui::GetFrameHeight() + style.ItemSpacing.x;
    return std::max(100.0f, width);
}

// A square button with a circular-arrow glyph drawn by hand (the overlay font has no icons).
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

// The zone pad: a miniature of the frame with the raw Work region (orange) and the 1:1 centre band
// (cyan); dragging inside it moves the band. Returns true when the offset changed.
bool DrawZonePad(pw::ConfigV2 &config, const pw::LayoutV2 *layout)
{
    const float nativeW = layout != nullptr && layout->nativeWidth != 0 ? static_cast<float>(layout->nativeWidth) : 3840.0f;
    const float nativeH = layout != nullptr && layout->nativeHeight != 0 ? static_cast<float>(layout->nativeHeight) : 2160.0f;
    const float rawWorkW = layout != nullptr && layout->rawWorkWidth != 0 ? static_cast<float>(layout->rawWorkWidth)
                                                                           : nativeW * config.xAxis.workPercent * 0.01f;
    const float rawWorkH = layout != nullptr && layout->rawWorkHeight != 0 ? static_cast<float>(layout->rawWorkHeight)
                                                                            : nativeH * config.yAxis.workPercent * 0.01f;
    const float limitX = pw::MaximumCenterOffsetPercentV2(config.xAxis.centerPercent);
    const float limitY = pw::MaximumCenterOffsetPercentV2(config.yAxis.centerPercent);

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
        if (newX != config.centerOffsetXPercent || newY != config.centerOffsetYPercent) {
            config.centerOffsetXPercent = newX;
            config.centerOffsetYPercent = newY;
            changed = true;
        }
    }

    ImDrawList *draw = ImGui::GetWindowDrawList();
    const ImVec2 end(origin.x + padW, origin.y + padH);
    draw->AddRectFilled(origin, end, IM_COL32(28, 28, 32, 255));
    draw->AddRect(origin, end, IM_COL32(120, 120, 130, 255));
    float cx0, cx1, wx0, wx1, cy0, cy1, wy0, wy1;
    ZoneAxisRectangles(nativeW, rawWorkW, config.xAxis.centerPercent * 0.01f, config.centerOffsetXPercent * 0.01f,
                       config.workShiftXPercent * 0.01f, &cx0, &cx1, &wx0, &wx1);
    ZoneAxisRectangles(nativeH, rawWorkH, config.yAxis.centerPercent * 0.01f, config.centerOffsetYPercent * 0.01f,
                       config.workShiftYPercent * 0.01f, &cy0, &cy1, &wy0, &wy1);
    const auto toPad = [&](float x, float y) { return ImVec2(origin.x + x / nativeW * padW, origin.y + y / nativeH * padH); };
    draw->AddRect(toPad(wx0, wy0), toPad(wx1, wy1), IM_COL32(255, 115, 0, 255), 0.0f, 0, 1.5f);
    draw->AddRectFilled(toPad(cx0, cy0), toPad(cx1, cy1), IM_COL32(0, 160, 200, 40));
    draw->AddRect(toPad(cx0, cy0), toPad(cx1, cy1), IM_COL32(0, 210, 255, 255), 0.0f, 0, 1.5f);
    const ImVec2 handle = toPad(0.5f * nativeW + config.centerOffsetXPercent * 0.01f * nativeW,
                                0.5f * nativeH + config.centerOffsetYPercent * 0.01f * nativeH);
    draw->AddCircleFilled(handle, 5.0f, active ? IM_COL32(255, 255, 255, 255) : IM_COL32(0, 210, 255, 255));
    draw->AddLine(ImVec2(origin.x + 0.5f * padW, origin.y), ImVec2(origin.x + 0.5f * padW, end.y), IM_COL32(90, 90, 100, 120));
    draw->AddLine(ImVec2(origin.x, origin.y + 0.5f * padH), ImVec2(end.x, origin.y + 0.5f * padH), IM_COL32(90, 90, 100, 120));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Drag to move the 1:1 zone (cyan); orange = raw Work region");
    return changed;
}

void DrawStatusBanner(const pw_ngx::Status &hook, const pw::ConfigV2 &config)
{
    if (pw_ngx::SafeMode()) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.3f, 1.0f), "CRASH GUARD: the previous session of this game ended right after the first frame this add-on warped (or was killed). Neural rendering runs untouched by this add-on now. Press Retry to warp again in this session; if the game dies again, the marker returns.");
        ImGui::PopTextWrapPos();
        if (ImGui::Button("Retry warping now")) CrashGuardRetry();
    }
    if (hook.active) {
        ImGui::TextColored(ImVec4(0.35f, 1.0f, 0.45f, 1.0f), "Optimizer FPS ACTIVE: model %ux%u of %ux%u, %llu frames%s%s",
                           hook.workWidth, hook.workHeight, hook.nativeWidth, hook.nativeHeight,
                           static_cast<unsigned long long>(hook.evaluations), hook.adopted ? " (model adopted)" : "",
                           hook.temporalMode == 1 ? ", temporal: interpolating" : hook.temporalMode == 2 ? ", temporal: centre every frame" : hook.temporalMode == 3 ? ", temporal: model in the background" : "");
        if (State().motionInvert || State().motionScaleAdjust != 1.0f) {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.3f, 1.0f), "WARNING: the motion vectors the model gets on the warped frame are %s%s (Advanced). This is a diagnostic and causes ghosting with the warp on; reset it unless you are testing the vector convention.",
                               State().motionInvert ? "inverted" : "", State().motionScaleAdjust != 1.0f ? (State().motionInvert ? " and scaled" : "scaled") : "");
            ImGui::PopTextWrapPos();
        }
    } else if (BridgeLinked() && !State().optiTakeover) {
        ImGui::TextColored(ImVec4(0.35f, 1.0f, 0.45f, 1.0f), "Optimizer FPS: applied by OptiScaler (see its menu for the status)");
    } else {
        const char *why = nullptr;
        if (!hook.moduleFound) why = "nvngx_dlssnr.dll is not loaded in this process (no Neural Rendering host)";
        else if (!hook.hooked) why = hook.reason[0] != 0 ? hook.reason : "hook not installed yet";
        else if (!hook.featureCreated) why = "waiting for the host to create feature 18";
        else if (config.mode == pw::WarpMode::Off) why = "mode is Off";
        else why = hook.reason[0] != 0 ? hook.reason : "pass-through";
        ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.15f, 1.0f), "Optimizer FPS NOT ACTIVE: %s", why);
        if (!hook.hooked && hook.moduleFound)
            ImGui::TextDisabled("hook attempts so far: %u (retried every frame)", hook.hookAttempts);
    }
}

void DrawOutputColour(int mode)
{
    if (mode != static_cast<int>(pw::WarpMode::Off)) {
        ImGui::SeparatorText("Output colour (warped frame only)");
        bool colorChanged = false;
        ImGui::SetNextItemWidth(ControlWidthFor("Brightness (%)", true));
        colorChanged |= ImGui::SliderFloat("##brightness", &State().brightnessPercent, -20.0f, 20.0f, "%.1f", ImGuiSliderFlags_AlwaysClamp);
        ImGui::SameLine();
        if (ResetIconButton("##brightness_reset", "Reset to 0 %")) { State().brightnessPercent = 0.0f; colorChanged = true; }
        ImGui::SameLine();
        ImGui::TextUnformatted("Brightness (%)");
        const float resetWidth = ImGui::CalcTextSize("Reset").x + ImGui::GetStyle().FramePadding.x * 2.0f + ImGui::GetStyle().ItemSpacing.x;
        ImGui::SetNextItemWidth(std::max(100.0f, ControlWidthFor("Gamma", false) - resetWidth));
        colorChanged |= ImGui::SliderFloat("##gamma", &State().gamma, 0.7f, 1.4f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
        ImGui::SameLine();
        ImGui::TextUnformatted("Gamma");
        ImGui::SameLine();
        if (ImGui::Button("Reset##colour")) {
            State().brightnessPercent = 0.0f;
            State().gamma = 1.0f;
            colorChanged = true;
        }
        if (colorChanged) SaveColorAdjustToReShadeIni();
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("Compensates the model's tone shift on the warped frame: colour = (1 + brightness) * rgb ^ (1 / gamma). Not applied without the warp.");
        ImGui::PopTextWrapPos();
    }
}

void DrawTemporal(const pw_ngx::Status &hook)
{
    // Temporal modes: the model does not have to run on every frame.
    {
        ImGui::SeparatorText("Temporal (Neural Rendering cadence)");
        pw_ngx::TemporalSettings &t = State().temporal;
        bool temporalChanged = false;
        const auto tslider = [&](const char *label, float *value, float lo, float hi, float defaultValue, const char *format, const char *tooltip = nullptr) {
            ImGui::SetNextItemWidth(ControlWidthFor(label, true));
            ImGui::PushID(label);
            bool edited = ImGui::SliderFloat("##slider", value, lo, hi, format, ImGuiSliderFlags_AlwaysClamp);
            if (tooltip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tooltip);
            ImGui::SameLine();
            char tip[128];
            std::snprintf(tip, sizeof(tip), "Reset to the default (%g)", defaultValue);
            if (ResetIconButton("##reset", tip)) { *value = defaultValue; edited = true; }
            ImGui::SameLine();
            ImGui::TextUnformatted(label);
            ImGui::PopID();
            return edited;
        };
        const auto tsliderInt = [&](const char *label, int *value, int lo, int hi, int defaultValue) {
            ImGui::SetNextItemWidth(ControlWidthFor(label, true));
            ImGui::PushID(label);
            bool edited = ImGui::SliderInt("##slider", value, lo, hi);
            ImGui::SameLine();
            char tip[128];
            std::snprintf(tip, sizeof(tip), "Reset to the default (%d)", defaultValue);
            if (ResetIconButton("##reset", tip)) { *value = defaultValue; edited = true; }
            ImGui::SameLine();
            ImGui::TextUnformatted(label);
            ImGui::PopID();
            return edited;
        };
        // Two modes that work (the centre-every-frame mode 2 was withdrawn on 2026-09-06; ini TemporalMode=2 falls back to 1).
        const char *shownModes[] = {"Every frame", "Interpolate: full NR every N-th frame (sync)", "Interpolate: model in the background (async)"};
        if (t.mode == 2) t.mode = 1;
        ImGui::SetNextItemWidth(ControlWidthFor("Temporal mode", false));
        int modeIndex = t.mode == 3 ? 2 : t.mode;
        if (ImGui::Combo("Temporal mode", &modeIndex, shownModes, 3)) { t.mode = modeIndex == 2 ? 3 : modeIndex; temporalChanged = true; }
        if (t.mode != 0) {
            if (t.mode == 3) {
                temporalChanged |= tsliderInt("Frames per model pass (N, 1 = continuous)", &t.every, 1, 8, 2);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("How often a new background pass starts. The model runs on its own GPU queue on copies of the inputs; every shown\nframe is the current colour plus the last finished pass's residual moved along the motion vectors. The residual's\nage is shown below; past 8 frames the host's queue waits for the pass.");
                temporalChanged |= tsliderInt("GPU frames queued ahead (0 = off)", &t.maxQueue, 0, 8, 2);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Before recording a frame the CPU waits until at most this many frames are still unfinished on the GPU (the\nlow-latency principle: the GPU stays busy, the queue stops piling up). The copies of a background pass then\nreach the GPU within this many frames instead of the host's whole queue (5-6 in BG3), which is most of the\nresidual's age. Also lowers input latency. The wait per frame is shown below.");
            } else {
                if (t.every < 2) t.every = 2;
                temporalChanged |= tsliderInt("Model pass every N frames", &t.every, 2, 8, 2);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Frames between full passes get the current colour plus the last pass's NR residual moved along the motion vectors.\nFrame times alternate (full / cheap); fine detail on moving objects refreshes at the full-pass rate.");
            }
        }
        if (temporalChanged) {
            t.every = std::clamp(t.every, t.mode == 3 ? 1 : 2, 8);
            SaveTemporalToReShadeIni();
            pw_ngx::SetTemporal(t);
        }
        if (hook.temporalReason[0] != 0 && t.mode != 0) {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.15f, 1.0f), "Temporal mode not applied: %s", hook.temporalReason);
            ImGui::PopTextWrapPos();
        } else if (t.mode == 3 && hook.temporalMode == 3) {
            ImGui::Text("Background model (%s queue): %.1f passes/s, %.1f ms per pass, residual age %u frames, passes %llu, forced waits %llu, stalls %llu",
                        hook.asyncQueue, hook.asyncPassesPerSecond, hook.asyncModelMs, hook.asyncAge, static_cast<unsigned long long>(hook.asyncPasses),
                        static_cast<unsigned long long>(hook.asyncForcedWaits), static_cast<unsigned long long>(hook.asyncStalls));
            if (t.maxQueue > 0)
                ImGui::Text("GPU queue cap: %.2f ms CPU wait per frame (smoothed), %llu frames waited", hook.asyncQueueWaitMs, static_cast<unsigned long long>(hook.asyncQueueWaits));
        } else if (t.mode != 0 && hook.temporalMode != 0) {
            ImGui::Text("Temporal: model frames %llu, interpolated %llu", static_cast<unsigned long long>(hook.fullFrames),
                        static_cast<unsigned long long>(hook.interpFrames));
        }
        if (hook.fallbackFrames > 0) {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.15f, 1.0f), "Frames shown as the plain colour (a stage failed): %llu, last: %s",
                               static_cast<unsigned long long>(hook.fallbackFrames), hook.fallbackReason);
            ImGui::PopTextWrapPos();
        }
        if (hook.modelSamplesFull > 0)
            ImGui::Text("Model GPU time: %.2f ms (n=%u)", hook.modelMsFull, hook.modelSamplesFull);
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
        int sourceIndex = OfaSettings().source == pw_ofa::SourceShader ? 2 : 1;
        ImGui::SetNextItemWidth(ControlWidthFor("Source", false));
        if (ImGui::Combo("Source", &sourceIndex, sources, 3)) {
            OfaSettings().source = sourceIndex == 2 ? pw_ofa::SourceShader : pw_ofa::SourceOfa;
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
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("Written to %s. The host re-reads it while it runs and reopens the optical flow session; "
                            "one frame after the change falls back to the shader vectors while the engine primes.",
                            pw_ofa::CfgPath().c_str());
        ImGui::PopTextWrapPos();
    }
}

void DrawDiagnostics(const pw_ngx::Status &hook, const pw::LayoutV2 &layout, bool layoutValid, pw::Status configStatus)
{
    if (ImGui::CollapsingHeader("Diagnostics")) {
        if (ImGui::TreeNode("Motion vectors handed to the model (not saved)")) {
            bool adjustChanged = false;
            ImGui::SetNextItemWidth(ControlWidthFor("Motion vector scale x", false));
            adjustChanged |= ImGui::InputFloat("Motion vector scale x", &State().motionScaleAdjust, 0.05f, 0.25f, "%.3f");
            adjustChanged |= ImGui::Checkbox("Invert motion vectors (wrong for BG3: ghosting with the warp on)", &State().motionInvert);
            if (State().motionInvert || State().motionScaleAdjust != 1.0f) { ImGui::SameLine(); if (ImGui::SmallButton("Reset to defaults")) { State().motionInvert = false; State().motionScaleAdjust = 1.0f; adjustChanged = true; } }
            if (adjustChanged && (!std::isfinite(State().motionScaleAdjust) || State().motionScaleAdjust == 0.0f)) State().motionScaleAdjust = 1.0f;
            if (hook.hooked && hook.featureCreated)
                ImGui::Text("Host motion: %ux%u, subrect %ux%u, MVecScale %.4f x %.4f (%s, getter slot %d)",
                            hook.hostMotionWidth, hook.hostMotionHeight, hook.hostMotionRectWidth, hook.hostMotionRectHeight,
                            hook.hostMotionScaleX, hook.hostMotionScaleY, hook.hostMotionScaleRead ? "read" : "NOT READ", hook.floatGetterSlot);
            ImGui::TreePop();
        }
        if (ImGui::TreeNode("Temporal machine (not saved)")) {
            pw_ngx::TemporalSettings &td = State().temporal;
            bool dc = false;
            ImGui::SetNextItemWidth(160.0f);
            dc |= ImGui::SliderFloat("Guided smoothing radius (px, 0 = off)", &td.smoothRadiusPx, 0.0f, 64.0f, "%.0f");
            dc |= ImGui::SliderFloat("Motion vector search radius (px, 0 = off)", &td.mvSearchRadiusPx, 0.0f, 64.0f, "%.0f");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Estimated (optical-flow) vectors are block-constant: a band of background around a near object carries the object's vector and reprojects onto it (silhouette halo). Within this radius the reprojection looks for a vector whose depth matches the pixel instead.");
            dc |= ImGui::Checkbox("Interpolated frames show the raw colour (no residual)", &td.debugRawInterpolation);
            {
                const char *views[] = { "Off", "Displacement / acceptance", "What the model changed (x4 around grey)", "Raw colour" };
                int view = std::clamp(td.debugView, 0, 3);
                if (ImGui::Combo("Debug view of interpolated frames", &view, views, 4)) { td.debugView = view; dc = true; }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("'What the model changed' shows the neural pass's contribution on the interpolated frames: flat grey = the model changes nothing visible; texture = it does, and the game's later passes may be hiding it.");
            }
            dc |= ImGui::Checkbox("Model gets single-frame vectors on full passes", &td.debugSingleFrameMotion);
            dc |= ImGui::Checkbox("Flip motion vector sign (temporal machine only)", &td.flipMotionSign);
            dc |= ImGui::Checkbox("Log the temporal state every 60 interpolated frames", &td.debugLog);
            if (dc) pw_ngx::SetTemporal(td);
            ImGui::TreePop();
        }
        if (hook.hooked) {
            ImGui::Text("NGX hook: %s, model %ux%u of %ux%u, warped %llu / passed %llu%s",
                        hook.active ? "Optimizer FPS active" : (hook.featureCreated ? "pass-through" : "waiting for feature 18"),
                        hook.workWidth, hook.workHeight, hook.nativeWidth, hook.nativeHeight,
                        static_cast<unsigned long long>(hook.evaluations), static_cast<unsigned long long>(hook.passthroughs),
                        hook.adopted ? ", model adopted" : "");
            if (hook.reason[0] != 0) ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.15f, 1.0f), "NGX hook: %s", hook.reason);
        } else {
            ImGui::Text("NGX hook: %s", hook.moduleFound ? hook.reason : "nvngx_dlssnr.dll not loaded in this process");
        }
    ImGui::Text("OptiScaler link: %s", !BridgeLinked() ? "not found (standalone; layout saved to ReShade.ini)"
        : State().optiTakeover ? (BridgeForcedWarpOff() ? "linked; OptiScaler's own warp is Off, this add-on warps inside its NR call (takeover)"
                                            : "linked; switching OptiScaler's own warp Off (takeover)...")
                         : "linked (OptiScaler applies the layout; saved to OptiScaler.ini and ReShade.ini)");
    if (BridgeLinked()) {
        bool takeover = State().optiTakeover;
        if (ImGui::Checkbox("Warp inside OptiScaler's NR call (takeover; off = OptiScaler's own warp, add-on features unavailable)", &takeover))
            SetOptiScalerTakeover(takeover);
    }
    if (BridgeLayoutRejected())
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.25f, 1.0f), "Layout rejected by OptiScaler; its values were restored.");
    if (configStatus != pw::Status::Ok)
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.25f, 1.0f), "Configuration rejected: %s", pw::StatusString(configStatus));
    if (layoutValid) {
        ImGui::Text("Native: %ux%u", layout.nativeWidth, layout.nativeHeight);
        ImGui::Text("Raw Work: %ux%u", layout.rawWorkWidth, layout.rawWorkHeight);
        ImGui::Text("Global scale: %.1f%%", layout.globalScalePercent);
        ImGui::Text("NR input: %ux%u (%.1f%% pixels)", layout.workWidth,
                    layout.workHeight, layout.pixelPercent);
        if ((layout.diagnosticFlags &
             (pw::LayoutDiagnosticAggressivePeripheralX |
              pw::LayoutDiagnosticAggressivePeripheralY)) != 0) {
            ImGui::TextColored(
                ImVec4(1.0f, 0.55f, 0.15f, 1.0f),
                "Aggressive peripheral compression; aliasing may increase (%.1fx / %.1fx max footprint)",
                layout.maximumSourceFootprintX, layout.maximumSourceFootprintY);
        }
    } else {
        ImGui::TextDisabled("Layout: waiting for feature 18 (no native size yet)");
    }
    }
}

void DrawOverlayBody(bool fitWindow)
{
    pw::Status configStatus = pw::Status::Ok;
    pw::ConfigV2 config = CurrentConfig(configStatus);

    // ReShade sizes this window to its content. Every control here is sized from the available
    // width, so without a floor the content and the window would shrink each other; an invisible
    // item of fixed width gives the content its minimum.
    constexpr float kMinimumContentWidth = 560.0f;
    ImGui::Dummy(ImVec2(kMinimumContentWidth, 0.0f));
    // Version banner. The exported NAME is version-free (see the top of this file), so this is the
    // only place in the overlay where the release number is shown.
    ImGui::TextDisabled("Optimizer FPS for DLSS5 %s", PW_ADDON_VERSION_STRING);
    // Status banner: whether the frame the model sees is actually warped right now.
    const pw_ngx::Status hook = pw_ngx::GetStatus();
    // The numbers shown below (and the zone pad) are the ones the interposer works with: the layout
    // this configuration produces at the native size of the host's feature. Before the host creates
    // feature 18 there is no native size, and nothing can be shown.
    pw::LayoutV2 layout{};
    const bool layoutValid = hook.nativeWidth != 0 && hook.nativeHeight != 0 &&
        pw::BuildLayout(config, hook.nativeWidth, hook.nativeHeight, &layout) == pw::Status::Ok;
    DrawStatusBanner(hook, config);
    ImGui::Separator();

    // Edits are applied when the control is released (sliders and the pad emit a value every frame
    // while dragging; each accepted change rebuilds the model or its GPU objects).
    bool changed = false;
    bool dragging = false;
    // Every slider carries a reset icon opposite it that restores the default value.
    const auto slider = [&](const char *label, float *value, float lo, float hi, float defaultValue, const char *format = "%.1f") {
        ImGui::SetNextItemWidth(ControlWidthFor(label, true));
        ImGui::PushID(label);
        const bool edited = ImGui::SliderFloat("##slider", value, lo, hi, format, ImGuiSliderFlags_AlwaysClamp);
        if (ImGui::IsItemActive()) dragging = true;
        ImGui::SameLine();
        char tip[96];
        std::snprintf(tip, sizeof(tip), "Reset to the default (%s)", format);
        char tipValue[128];
        std::snprintf(tipValue, sizeof(tipValue), tip, defaultValue);
        const bool reset = ResetIconButton("##reset", tipValue);
        ImGui::SameLine();
        ImGui::TextUnformatted(label);
        ImGui::PopID();
        if (reset) *value = defaultValue;
        changed |= edited || reset;
    };
    int mode = static_cast<int>(config.mode);
    const char *modes[] = {"Off", "Uniform", "Peripheral"};
    ImGui::SetNextItemWidth(ControlWidthFor("Mode", false));
    if (ImGui::Combo("Mode", &mode, modes, 3)) { config.mode = static_cast<pw::WarpMode>(mode); changed = true; }
    int colorFilter = static_cast<int>(config.colorFilter);
    const char *colorFilters[] = {"Bilinear", "Auto (soft: wide pre-filter, cubic unpack)"};
    ImGui::SetNextItemWidth(ControlWidthFor("Color filter", false));
    if (ImGui::Combo("Color filter", &colorFilter, colorFilters, 2)) {
        config.colorFilter = static_cast<pw::ColorFilter>(colorFilter);
        changed = true;
    }
    if (mode != static_cast<int>(pw::WarpMode::Off)) {
        ImGui::SeparatorText("Zone size");
        if (mode == static_cast<int>(pw::WarpMode::Peripheral)) {
            slider("Center X (%)", &config.xAxis.centerPercent, 1.0f, 99.0f, 80.0f);
            slider("Center Y (%)", &config.yAxis.centerPercent, 1.0f, 99.0f, 80.0f);
        }
        slider("Work X (%)", &config.xAxis.workPercent, pw::kMinimumWorkPercentV2, 100.0f, 90.0f);
        slider("Work Y (%)", &config.yAxis.workPercent, pw::kMinimumWorkPercentV2, 100.0f, 90.0f);
    }
    slider("Global scale (%)", &config.globalScalePercent, pw::kMinimumEffectiveScalePercentV2, 100.0f, 100.0f);
    if (mode == static_cast<int>(pw::WarpMode::Peripheral)) {
        ImGui::SeparatorText("Zone position");
        const float limitX = pw::MaximumCenterOffsetPercentV2(config.xAxis.centerPercent);
        const float limitY = pw::MaximumCenterOffsetPercentV2(config.yAxis.centerPercent);
        config.centerOffsetXPercent = std::clamp(config.centerOffsetXPercent, -limitX, limitX);
        config.centerOffsetYPercent = std::clamp(config.centerOffsetYPercent, -limitY, limitY);
        if (DrawZonePad(config, layoutValid ? &layout : nullptr)) changed = true;
        if (ImGui::IsItemActive()) dragging = true;
        // One slider per axis, full width, with a reset-to-centre button opposite each; a double
        // click on the slider itself also centres that axis.
        const auto offsetSlider = [&](const char *label, const char *resetId, float *value, float limit) {
            ImGui::SetNextItemWidth(ControlWidthFor(label, true));
            ImGui::PushID(resetId);
            const bool edited = ImGui::SliderFloat("##slider", value, -limit, limit, "%.1f", ImGuiSliderFlags_AlwaysClamp);
            const bool doubleClicked = ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
            if (ImGui::IsItemActive() && !doubleClicked) dragging = true;
            ImGui::SameLine();
            const bool reset = ResetIconButton("##reset", "Reset this axis to the centre (double-click the slider does the same)");
            ImGui::SameLine();
            ImGui::TextUnformatted(label);
            ImGui::PopID();
            if (reset || doubleClicked) {
                *value = 0.0f;
                changed = true;
            } else {
                changed |= edited;
            }
        };
        offsetSlider("Offset X (%)", "##resetx", &config.centerOffsetXPercent, limitX);
        offsetSlider("Offset Y (%)", "##resety", &config.centerOffsetYPercent, limitY);
        if (ImGui::Button("Center both")) {
            config.centerOffsetXPercent = 0.0f;
            config.centerOffsetYPercent = 0.0f;
            changed = true;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("Range X +-%.1f%%, Y +-%.1f%%", limitX, limitY);
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextWrapped("Work size stays the same: the wider side is compressed harder, the narrower one never below 1:1.");
        ImGui::PopTextWrapPos();

        // Work shift: slide the outer (orange) contour while the band stays; the compression
        // budget moves between the two sides of each axis.
        if (ImGui::Checkbox("Shift the compressed region (outer contour)", &State().workShiftEnabled)) {
            SaveWorkShiftEnabledToReShadeIni();
            if (!State().workShiftEnabled && (config.workShiftXPercent != 0.0f || config.workShiftYPercent != 0.0f)) {
                config.workShiftXPercent = 0.0f;
                config.workShiftYPercent = 0.0f;
                changed = true;
            }
        }
        if (State().workShiftEnabled) {
            float shiftMinX = 0.0f, shiftMaxX = 0.0f, shiftMinY = 0.0f, shiftMaxY = 0.0f;
            pw::WorkShiftLimitsPercentV2(config, 0, &shiftMinX, &shiftMaxX);
            pw::WorkShiftLimitsPercentV2(config, 1, &shiftMinY, &shiftMaxY);
            config.workShiftXPercent = std::clamp(config.workShiftXPercent, shiftMinX, shiftMaxX);
            config.workShiftYPercent = std::clamp(config.workShiftYPercent, shiftMinY, shiftMaxY);
            const auto shiftSlider = [&](const char *label, const char *resetId, float *value, float lo, float hi) {
                ImGui::SetNextItemWidth(ControlWidthFor(label, true));
                ImGui::PushID(resetId);
                const bool edited = ImGui::SliderFloat("##slider", value, lo, hi, "%.1f", ImGuiSliderFlags_AlwaysClamp);
                const bool doubleClicked = ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
                if (ImGui::IsItemActive() && !doubleClicked) dragging = true;
                ImGui::SameLine();
                const bool reset = ResetIconButton("##reset", "Reset this axis (double-click the slider does the same)");
                ImGui::SameLine();
                ImGui::TextUnformatted(label);
                ImGui::PopID();
                if (reset || doubleClicked) {
                    *value = 0.0f;
                    changed = true;
                } else {
                    changed |= edited;
                }
            };
            shiftSlider("Work shift X (%)", "##resetwx", &config.workShiftXPercent, shiftMinX, shiftMaxX);
            shiftSlider("Work shift Y (%)", "##resetwy", &config.workShiftYPercent, shiftMinY, shiftMaxY);
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextDisabled("Range X %.1f..%.1f%%, Y %.1f..%.1f%% (the side the contour moves towards is compressed less, the opposite side more)",
                                shiftMinX, shiftMaxX, shiftMinY, shiftMaxY);
            ImGui::PopTextWrapPos();
        }
        ImGui::SeparatorText("Outlines");
        bool outlinesChanged = false;
        outlinesChanged |= ImGui::Checkbox("Show uncompressed center (cyan)", &State().showCenterOutline);
        ImGui::SameLine();
        outlinesChanged |= ImGui::Checkbox("Show Work boundary (orange)", &State().showWorkOutline);
        if (outlinesChanged) SaveOutlinesToReShadeIni();
    } else if (mode == static_cast<int>(pw::WarpMode::Uniform)) {
        ImGui::TextDisabled("Zone position and outlines are available in Peripheral mode only.");
    }
    DrawOutputColour(mode);
    DrawTemporal(hook);
    DrawOfa();

    bool extendMotion = (config.flags & pw::ConfigFlagExtendMotionAtEdge) != 0;
    if (ImGui::Checkbox("Extend motion beyond frame edge", &extendMotion)) {
        if (extendMotion) config.flags |= pw::ConfigFlagExtendMotionAtEdge;
        else config.flags &= ~pw::ConfigFlagExtendMotionAtEdge;
        changed = true;
    }
    // While a slider or the pad is being dragged the pending value is parked and applied on release.
    static bool s_pending = false;
    static pw::ConfigV2 s_pendingConfig{};
    if (changed && dragging) {
        s_pending = true;
        s_pendingConfig = config;
    } else if (changed) {
        s_pending = false;
        ApplyConfig(config);
    } else if (s_pending && !dragging) {
        s_pending = false;
        ApplyConfig(s_pendingConfig);
    }
    ImGui::Spacing();

    DrawDiagnostics(hook, layout, layoutValid, configStatus);
    // The overlay is a separate ReShade window whose height the user cannot easily keep in step
    // with the content: fit the height to what was drawn. The width stays the user's, but never
    // below a usable minimum (a freshly created ImGui window starts almost zero wide, and pinning
    // that width once made the tab a 20-pixel strip).
    if (!fitWindow) return; // embedded in ReShade's Add-ons tab: never resize the host window
    const float width = std::max(kMinimumContentWidth + 2.0f * ImGui::GetStyle().WindowPadding.x, ImGui::GetWindowWidth());
    ImGui::SetWindowSize(ImVec2(width, ImGui::GetCursorPosY() + ImGui::GetStyle().WindowPadding.y), ImGuiCond_Always);
}
} // namespace

void DrawOverlay(reshade::api::effect_runtime *) { DrawOverlayBody(true); }
void DrawOverlayEmbedded(reshade::api::effect_runtime *) { DrawOverlayBody(false); }

} // namespace pw_addon
