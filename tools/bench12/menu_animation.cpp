#include "menu.h"
#include "external/imgui/imgui.h"

bool Menu::AnimationControls(Device &d,Scene &scene,Options &o)
{
    bool changed=ImGui::Checkbox("Play",&o.animPlaying);
    changed|=ImGui::SliderFloat("Animation speed",&o.animSpeed,-4,4,"%.2f");
    ImGui::BeginDisabled(scene.animationDuration<=0);
    if(ImGui::SliderFloat("Time",&scene.animationTime,0,scene.animationDuration,"%.3f s")) {
        scene.animationSeek=true; changed=true;
    }
    if(ImGui::Button("Step one frame")) { o.animPlaying=false; scene.animationStep=true; changed=true; }
    ImGui::EndDisabled();
    Vec3 position{},heading{}; bool available=scene.FollowPose(scene.animationTime,position,heading);
    bool follow=o.camera=="follow";
    ImGui::BeginDisabled(!available);
    if(ImGui::Checkbox("Follow character",&follow)) { o.camera=follow ? "follow" : "static"; changed=true; }
    ImGui::EndDisabled();
    if(!available) ImGui::TextUnformatted("No follow node (use --follow-node NAME).");
    ImGui::Text("Dynamic triangles: %u",scene.dynamicTriangles);
    ImGui::Text("CPU skinning %.3f ms | BLAS refit/rebuild %.3f ms",scene.animationCpuMs,scene.dynamicTriangles ? d.lastBlasMs : 0);
    return changed;
}
