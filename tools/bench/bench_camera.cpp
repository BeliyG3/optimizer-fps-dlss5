#include "bench_camera.h"

#include "bench_options.h"
#include "pw_gltf.h"

#include <cstdio>
#include <cstring>
#include <string>

CameraState CameraAt(int frame, const char *mode)
{
    const int phaseLength = 120;
    int phase;
    if (strcmp(mode, "static") == 0) phase = 0;
    else if (strcmp(mode, "yaw") == 0) phase = 1;
    else if (strcmp(mode, "forward") == 0) phase = 2;
    else if (strcmp(mode, "strafe") == 0) phase = 3;
    else if (strcmp(mode, "combo") == 0) phase = 4;
    else phase = (frame / phaseLength) % 5;
    // Integrate the scripted motion up to this frame so phases chain continuously.
    float yaw = 0.0f, forward = 0.0f, strafe = 0.0f;
    const float yawRate = g_yawSpeed / 60.0f * 3.14159265f / 180.0f; // deg/s -> rad/frame
    const float moveRate = 2.0f / 60.0f;                        // 2 m/s
    auto step = [&](int p) {
        if (p == 1) yaw += yawRate;
        else if (p == 2) forward += moveRate;
        else if (p == 3) strafe += moveRate;
        else if (p == 4) { yaw += yawRate * 0.6f; forward += moveRate * 0.7f; }
    };
    if (strcmp(mode, "script") == 0) {
        for (int f = 0; f < frame; ++f) step((f / phaseLength) % 5);
    } else {
        for (int f = 0; f < frame; ++f) step(phase);
    }
    // Yaw rotates the camera around the figure; forward/strafe move camera and figure together
    // (the figure walks), so the boxes flow past.
    const Vec3 dir{std::sin(yaw), 0.0f, std::cos(yaw)};
    const Vec3 right{std::cos(yaw), 0.0f, -std::sin(yaw)};
    const Vec3 base = dir * forward + right * strafe;
    CameraState c;
    c.target = base + Vec3{0.0f, 1.0f, 0.0f};
    c.eye = c.target - dir * 5.0f + Vec3{0.0f, 0.9f, 0.0f};
    return c;
}

CameraState ChooseCamera(const BenchOptions &o, const pwgltf::Scene &gscene, int frame, float &fovY)
{
    CameraState cam;
    fovY = 60.0f;
    if (o.camPosGiven && o.camTargetGiven) {
        // --cam-pos / --cam-target / --fov (and the --camera blender preset): a fixed camera in
        // glTF Y-up metres, scaled into the scene's own units by --scene-scale.
        cam.eye = o.camPosM * o.sceneScale;
        cam.target = o.camTargetM * o.sceneScale;
        if (o.fovGiven && o.camFov > 0.0f) fovY = o.camFov;
        static bool s_logCam = false;
        if (!s_logCam) { s_logCam = true; std::printf("[info] camera override: eye (%.2f, %.2f, %.2f) target (%.2f, %.2f, %.2f) in scene units (scale %.0f), fovY %.4f deg, shift (%.3f, %.3f)\n",
                                                      cam.eye.x, cam.eye.y, cam.eye.z, cam.target.x, cam.target.y, cam.target.z, o.sceneScale, fovY, o.camShift.x, o.camShift.y); }
    } else if (o.gltfPath && strcmp(o.cameraMode, "file") == 0) {
        if (!pwgltf::CameraAt(gscene, frame * o.animSpeed / 60.0f, cam.eye, cam.target, fovY)) cam = CameraAt(frame, "script");
    } else if (o.gltfPath && strcmp(o.cameraMode, "face") == 0) {
        // Head = bounding box of the meshes under the face node at this instant (morphs and node animation
        // included), sweeping slowly around its centre; the radius follows the head's size unless given.
        static int s_anchor = -2;
        if (s_anchor == -2) {
            s_anchor = -1;
            for (size_t i = 0; i < gscene.nodes.size() && s_anchor < 0; ++i) if (gscene.nodes[i].name == o.faceNode) s_anchor = (int) i;
            for (size_t i = 0; i < gscene.nodes.size() && s_anchor < 0; ++i) { const std::string &n = gscene.nodes[i].name; if (n.find("girl") != std::string::npos || n.find("haracter") != std::string::npos || n.find("head") != std::string::npos) s_anchor = (int) i; }
            if (s_anchor >= 0) std::printf("[info] face camera: node %d (%s)\n", s_anchor, gscene.nodes[s_anchor].name.c_str());
            else std::printf("[warn] face camera: node %s not found; the scripted camera is used\n", o.faceNode);
        }
        bool placed = false;
        if (s_anchor >= 0) {
            const float t = frame * o.animSpeed / 60.0f;
            Vec3 lo{0, 0, 0}, hi{0, 0, 0};
            pwgltf::MeshBounds(gscene, t, s_anchor, lo, hi); // morphs + skinning applied
            if (lo.x < hi.x) {
                fovY = 35.0f;
                const Vec3 head = (lo + hi) * 0.5f;
                const float height = hi.y - lo.y, depth = hi.z - lo.z;
                // Auto radius (--face-radius 0): the distance at which the head's height fills
                // kFaceFill of the frame (frame height at distance d is 2*d*tan(fovY/2), so
                // d = 0.5*height/tan(fovY/2)/kFaceFill). The silhouette extremes - the top of the
                // hair and the chin - sit near the box's centre plane, which is what the camera
                // orbits, so that is the plane the fill is computed for; half the box's depth is
                // the floor, so the near cheek can never end up behind the camera on a wide head.
                // kFaceAim then drops the aim point below the box centre until the eye line lands
                // at ~40% from the top of the frame (the eye line itself is near centre + 0.03 x
                // height on this box, which reaches well below the chin).
                const float kFaceFill = 0.75f, kFaceAim = -0.10f;
                const float fit = 0.5f * height / std::tan(fovY * 3.14159265f / 360.0f) / kFaceFill;
                const float radius0 = o.faceRadius > 0.0f ? o.faceRadius : std::max(fit, 0.5f * depth);
                static bool s_logged = false;
                if (!s_logged) { s_logged = true; std::printf("[info] face camera: head %.2f x %.2f x %.2f, centre (%.1f, %.1f, %.1f), radius %.2f (%s), fill %.0f%%, angle %.1f deg\n",
                                                              hi.x - lo.x, height, depth, head.x, head.y, head.z, radius0, o.faceRadius > 0.0f ? "given" : "auto", kFaceFill * 100.0f, o.faceAngle); }
                // --face-angle turns the camera off the face axis so the head reads slightly turned.
                const float a = (o.faceYaw + o.faceAngle + o.faceSweep * std::sin(t * 0.6f)) * 3.14159265f / 180.0f; // slow sweep (0 = still)
                const float r = radius0 * (1.0f + (o.faceSweep > 0.0f ? 0.08f : 0.0f) * std::sin(t * 0.37f)); // and a slight dolly
                const Vec3 aim = head + Vec3{0.0f, kFaceAim * height, 0.0f};
                cam.eye = aim + Vec3{std::sin(a) * r, 0.0f, std::cos(a) * r}; // level with the eye line
                cam.target = aim;
                placed = true;
            }
        }
        if (!placed) cam = CameraAt(frame, "script");
    } else {
        cam = CameraAt(frame, o.cameraMode);
        // The scripted path is "around a figure at the origin": in a glTF scene the figure is where
        // its author put it, so the path moves there (the floor position of the first node named
        // like a character).
        static int s_figure = -2;
        static Vec3 s_figureAt{0, 0, 0};
        if (s_figure == -2) {
            s_figure = -1;
            for (size_t i = 0; i < gscene.nodes.size() && s_figure < 0; ++i) { const std::string &n = gscene.nodes[i].name; if (gscene.nodes[i].mesh >= 0 && (n.find("girl") != std::string::npos || n.find("haracter") != std::string::npos)) s_figure = (int) i; }
            Vec3 lo{0, 0, 0}, hi{0, 0, 0};
            if (s_figure >= 0 && pwgltf::MeshBounds(gscene, 0.0f, s_figure, lo, hi) && lo.x < hi.x) {
                s_figureAt = Vec3{(lo.x + hi.x) * 0.5f, 0.0f, (lo.z + hi.z) * 0.5f};
                std::printf("[info] scripted camera follows %s at (%.2f, %.2f)\n", gscene.nodes[s_figure].name.c_str(), s_figureAt.x, s_figureAt.z);
            }
        }
        cam.eye = cam.eye + s_figureAt;
        cam.target = cam.target + s_figureAt;
    }
    // --cam-dolly M: whichever camera was chosen, moved M scene units towards its target (never
    // past 0.3 of the way left), so a scripted third-person shot can be brought in on the character.
    if (o.camDolly != 0.0f) {
        const Vec3 to = cam.target - cam.eye;
        const float distance = std::sqrt(to.x * to.x + to.y * to.y + to.z * to.z);
        if (distance > 1e-4f) cam.eye = cam.eye + to * (std::min(o.camDolly, distance * 0.7f) / distance);
    }
    if (o.camLift != 0.0f) {
        cam.eye.y += o.camLift;
        cam.target.y += o.camLift;
    }
    // --sway: the slow drift of a third-person camera behind a standing character (two incommensurate
    // sines per axis). The player "stands still" and the whole picture moves a few pixels a frame.
    if (o.sway != 0.0f) {
        const float ts = frame / 60.0f;
        cam.eye.x += o.sway * (std::sin(ts * 0.83f) + 0.5f * std::sin(ts * 1.91f));
        cam.eye.y += o.sway * 0.6f * (std::sin(ts * 0.57f + 1.3f) + 0.5f * std::sin(ts * 1.37f));
    }
    return cam;
}
