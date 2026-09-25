#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>
#include "overlay.h"
#include "config_store.h"
#include "../shell_host.h"
#include "../ngx_hook_api.h"
namespace ofps::reshade {
namespace { const char *Str(const char *s) { return s ? s : ""; } }
void DrawDiagnostics(const ofps::ui::UiSnapshot &snapshot)
{
    const OfpsStatus &hook = snapshot.status;
    const bool directHostActive = snapshot.directHost;
    const auto hookInfo = GetHookStatus();
    const OfpsLayoutPreview &layout = snapshot.preview;
    const bool layoutValid = layout.nativeW && layout.nativeH;
    ofps::sdk::Status configStatus{}; CurrentConfig(configStatus);
    if (ImGui::CollapsingHeader("Host diagnostics")) {
        if (hookInfo.hooked) {
            ImGui::Text("NGX hook: %s, model %ux%u of %ux%u, warped %llu / passed %llu%s",
                        hook.active ? "Optimizer FPS active" : (hook.featureCreated ? "pass-through" : "waiting for feature 18"),
                        hook.workW, hook.workH, hook.nativeW, hook.nativeH,
                        static_cast<unsigned long long>(hook.evaluations), static_cast<unsigned long long>(hook.passthroughs),
                        hook.adopted ? ", model adopted" : "");
            if (Str(hook.reason)[0] != 0) ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.15f, 1.0f), "NGX hook: %s", Str(hook.reason));
        } else {
            ImGui::Text("NGX hook: %s", hookInfo.moduleFound ? Str(hook.reason) : "nvngx_dlssnr.dll not loaded in this process");
        }
    ImGui::TextUnformatted(directHostActive ? "Direct host: active (forward latched)" : "Direct host: inactive");
    if (const char *error = CoreLoadError(); *error) ImGui::TextWrapped("%s", error);
    if (configStatus != ofps::sdk::Status::Ok)
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.25f, 1.0f), "Configuration rejected: %s", ofps::sdk::StatusString(configStatus));
    if (layoutValid) {
        ImGui::Text("Native: %ux%u", layout.nativeW, layout.nativeH);
        ImGui::Text("Raw Work: %ux%u", layout.rawWorkW, layout.rawWorkH);
        if (snapshot.values.count == OFPS_SET_COUNT)
            ImGui::Text("Global scale: %.1f%%", snapshot.values.v[OFPS_SET_GLOBAL_SCALE].f);
        ImGui::Text("NR input: %ux%u (%.1f%% pixels)", layout.modelW,
                    layout.modelH, layout.pixelPercent);
        if ((layout.diagnosticFlags &
             (ofps::sdk::LayoutDiagnosticAggressivePeripheralX |
              ofps::sdk::LayoutDiagnosticAggressivePeripheralY)) != 0) {
            ImGui::TextColored(
                ImVec4(1.0f, 0.55f, 0.15f, 1.0f),
                "Aggressive peripheral compression; aliasing may increase (%.1fx / %.1fx max footprint)",
                layout.maxSourceFootprintX, layout.maxSourceFootprintY);
        }
    } else {
        ImGui::TextDisabled("Layout: waiting for feature 18 (no native size yet)");
    }
    }
}

}
