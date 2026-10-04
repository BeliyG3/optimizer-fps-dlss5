#include "bench_options.h"

#include "bench_camera.h"
#include "bench_hdri.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

// Usage: pw_bench [frames] [options]
//   --scene 3d|flat        3D: textured ground, boxes, a figure at the orbit centre, third-person camera
//                          (default); flat: the old moving checkerboard.
//   --yaw-speed D        camera yaw rate in degrees per second for the yaw phases (default 45)
//   --fps-cap N          pace the frames to N per second on the CPU (spin-wait), giving the GPU idle time as a game would
//   --frametime          print statistics of the Present-to-Present intervals (frames 60..end): avg, min, max,
//                        standard deviation, share of frames longer than 1.5x the average
//   --gltf FILE.glb      render a glTF scene (Blender export of tools/bench/assets/bench_scene.blend) instead of the
//                        procedural one; node animations drive the objects and the file's camera drives the view
//                        (use --camera to override with the scripted path). Blender's stripe materials
//                        ("Palette_*") keep their stripes.
//   --camera face          (--gltf) close-up orbit around the head: the bounding box of the meshes under the node
//                          --face-node NAME (default "CharacterAnchor", else the first node whose name contains
//                          "girl"/"character"/"head"), recomputed every frame; a slow +-25 deg sweep, 35 deg FOV;
//                          --face-yaw D turns the sweep's centre (degrees, 0 = in front of a face looking along +Z),
//                          --face-angle D the camera's horizontal offset from that axis so the head reads slightly
//                          turned (default -15: turned right-to-left; +15 mirrors it), --face-radius R the
//                          distance (scene units; 0 = auto, the head's height filling 75% of the frame),
//                          --face-sweep A the sweep amplitude (0 = still)
//   --camera blender       the look tuned in Blender for the cutegirl head, baked in: --cam-pos/-target/--fov,
//                          --cam-shift, --sun-dir/--sun-strength, --scene-scale 100, --hdri-yaw -90 and
//                          --scene-yaw 180 as defaults (each still overridable). Nothing is read from disk.
//   --cam-pos x,y,z        fixed camera, glTF Y-up, in the units --scene-scale converts from (metres by
//   --cam-target x,y,z     default). Overrides every --camera path when both are given.
//   --fov D                vertical field of view in degrees for the camera override (Blender's 57 mm lens
//                          on a 16:9 frame is 23.7773).
//   --cam-shift x,y        Blender's camera shift_x,shift_y for the camera override, in units of the frame's
//                          height (its "VERTICAL" sensor fit). Off-centre framing without turning the camera.
//   --scene-scale S        multiplies --cam-pos/--cam-target into the scene's own units (default 1; the
//                          cutegirl .glb is authored in centimetres, so metres x 100 -> S = 100).
//   --scene-yaw D          the loaded asset is turned D degrees about +Y relative to the scene the camera,
//                          sun and HDRI numbers were authored in; the camera override, --sun-dir, --hdri-yaw
//                          and --camera face's orbit are all turned by D to compensate (default 0; 180 for
//                          --camera blender, because cutegirl.glb was re-exported facing glTF +Z while the
//                          .blend the look was tuned in has it facing -Z).
//   --sun-dir x,y,z        directional light, glTF Y-up, the direction the light TRAVELS (Blender's own
//                          convention); the shader's lightDir is its negation. Bypasses the HDRI sun search,
//                          and is NOT turned by --hdri-yaw (it is already in world space).
//   --sun-strength E       sun irradiance in the HDRI's radiance units (Blender's W/m^2 at HDRI strength 1);
//                          the shader gets E/pi, white. Bypasses --sun-scale / kSunOverSky.
//   --anim-speed S         glTF animation playback factor (default 1).
//   --shadow-soft W        penumbra half-width as a fraction of the shadow frustum's width (default 0.006);
//                          16-tap Poisson PCF with a per-pixel rotation, radius clamped to 1..48 texels.
//   --camera script|static|yaw|forward|strafe|combo   camera path for the 3D scene (script = all
//                          phases, 120 frames each, looping).
//   --moving-boxes         two boxes move on their own (object motion vectors).
//   --reference            no DLSS/NR: the 3D scene is rendered straight at 3840x2160 with 8 jitter
//                          samples averaged; --dump writes these frames (ground truth for metrics).
//   --hdri FILE.hdr|none   environment lighting for the 3D scene from a Radiance equirect HDRI: sky
//                          background, diffuse ambient and the sun's direction/colour come from it
//                          (default: assets/external/golden_gate_hills_2k.hdr when it exists).
//   --hdri-yaw D|auto      turn the environment (sky, ambient and the extracted sun alike) about the vertical
//                          axis. "auto" (default) puts the sun 40 deg to the side of the face camera's axis, a
//                          front-side key light; it resolves to 0 for every other camera and to 90 for
//                          --camera blender (before --scene-yaw is added to it). BLENDER PARITY:
//                          Blender's equirect world is ours turned a quarter turn - u_blender = u_ours - 0.25,
//                          v identical, no mirror - and the Mapping node turns the LOOKUP VECTOR while our
//                          yaw turns the MAP, so the senses are opposite:
//                                            ourYaw = -90 deg - blenderYaw.
//                          Measured, not guessed: an equirect probe image carrying R = u, G = v was put in
//                          the .blend's world and rendered through its own camera with Cycles (shift zeroed,
//                          view transform Raw). Reading Blender's (u,v) off every pixel and comparing it with
//                          EquirectUV for the same world direction gives du = -0.2512 +- 0.0016 (-90.44 deg,
//                          the spread is RGBE quantisation) and dv = -0.002 +- 0.001. u grows to the right in
//                          both, so the mappings are a pure rotation apart. The sun lamp is NOT a witness
//                          here: the user aimed it by eye, ~100 deg off the HDRI's own sun.
//   --sun-scale F          sun irradiance as a multiple of the HDRI's mean radiance (default 1.2: a soft,
//                          low-contrast key). The measured sun disk is normalised to this.
//   --exposure F           linear multiplier on the scene radiance (sky, ambient, sun) before the
//                          Khronos PBR Neutral tone mapper (default 1.45).
//   --switch N             every N frames cycle the NR add-on's layout Off -> Uniform -> Peripheral.
//   --temporal M[,N]       NR add-on temporal mode M with a full pass every N frames (from frame 10).
//   --dump A[,B,...]       write frame A, B, ... of the output as full-size BMP (dump_<n>.bmp).
//   --measure N            every N frames print the mean luminance of the output.
//   --mv-scale S           MV_Scale handed to DLSS (BG3 uses -1); stored vectors are divided by it.
//   --mv-dir +1|-1         sign of the stored vectors: +1 = NGX convention (current -> previous).
//   --present WxH          swap chain size (the DLSS output stays 4K).
//   --nr-pause A,B[,A,B...] frames A..B-1 (half-open, ascending pairs) skip the DLSS evaluate, so the bridge and
//                          renodx run no NR, and present the render-size colour instead (a menu drawn without NR).
//   --dump-back            --dump writes the back buffer just before Present (32-bit top-down BMP, alpha 255, the
//                          layout of the add-on's menu dumps) instead of the DLSS output.
//   --debug-layer          D3D12 debug layer and a D3D11 debug device; the D3D11 messages are listed at the end.

bool ParseOptions(int argc, char **argv, BenchOptions &o)
{
    auto parseVec3 = [](const char *s, Vec3 &out) { float a = 0, b = 0, c = 0; if (sscanf_s(s, "%f,%f,%f", &a, &b, &c) == 3) { out = {a, b, c}; return true; } return false; };
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--switch") == 0 && i + 1 < argc) o.switchEvery = atoi(argv[++i]);
        else if (strcmp(argv[i], "--temporal") == 0 && i + 1 < argc) { unsigned m = 0, n = 2; if (sscanf_s(argv[++i], "%u,%u", &m, &n) >= 1) { o.temporalMode = (int) m; o.temporalEvery = (int) n; } }
        else if (strcmp(argv[i], "--dump") == 0 && i + 1 < argc) { for (char *tok = strtok(argv[++i], ","); tok; tok = strtok(nullptr, ",")) o.dumpFrames.push_back(atoi(tok)); }
        else if (strcmp(argv[i], "--nr-pause") == 0 && i + 1 < argc) { for (char *tok = strtok(argv[++i], ","); tok; tok = strtok(nullptr, ",")) o.nrPause.push_back(atoi(tok)); }
        else if (strcmp(argv[i], "--dump-back") == 0) o.dumpBack = true;
        else if (strcmp(argv[i], "--debug-layer") == 0) o.debugLayer = true;
        else if (strcmp(argv[i], "--mv-scale") == 0 && i + 1 < argc) o.mvScale = (float) atof(argv[++i]);
        else if (strcmp(argv[i], "--mv-dir") == 0 && i + 1 < argc) o.mvDir = (float) atof(argv[++i]);
        else if (strcmp(argv[i], "--measure") == 0 && i + 1 < argc) o.measureEvery = atoi(argv[++i]);
        else if (strcmp(argv[i], "--present") == 0 && i + 1 < argc) { unsigned w = 0, h = 0; if (sscanf_s(argv[++i], "%ux%u", &w, &h) == 2 && w && h) { o.presentW = w; o.presentH = h; } }
        else if (strcmp(argv[i], "--scene") == 0 && i + 1 < argc) o.scene = argv[++i];
        else if (strcmp(argv[i], "--camera") == 0 && i + 1 < argc) { o.cameraMode = argv[++i]; o.cameraGiven = true; }
        else if (strcmp(argv[i], "--face-yaw") == 0 && i + 1 < argc) o.faceYaw = (float) atof(argv[++i]);
        else if (strcmp(argv[i], "--face-radius") == 0 && i + 1 < argc) o.faceRadius = (float) atof(argv[++i]);
        else if (strcmp(argv[i], "--cam-dolly") == 0 && i + 1 < argc) o.camDolly = (float) atof(argv[++i]);
        else if (strcmp(argv[i], "--hdr") == 0) o.hdrOutput = true;
        else if (strcmp(argv[i], "--ambient") == 0 && i + 1 < argc) o.ambientScale = (float) atof(argv[++i]);
        else if (strcmp(argv[i], "--albedo") == 0 && i + 1 < argc) o.albedoScale = (float) atof(argv[++i]);
        else if (strcmp(argv[i], "--boil") == 0 && i + 1 < argc) o.boil = (float) atof(argv[++i]);
        else if (strcmp(argv[i], "--sway") == 0 && i + 1 < argc) o.sway = (float) atof(argv[++i]);
        else if (strcmp(argv[i], "--depth") == 0 && i + 1 < argc) g_standardDepth = strcmp(argv[++i], "standard") == 0;
        else if (strcmp(argv[i], "--cam-lift") == 0 && i + 1 < argc) o.camLift = (float) atof(argv[++i]);
        else if (strcmp(argv[i], "--face-sweep") == 0 && i + 1 < argc) o.faceSweep = (float) atof(argv[++i]);
        else if (strcmp(argv[i], "--face-angle") == 0 && i + 1 < argc) o.faceAngle = (float) atof(argv[++i]);
        else if (strcmp(argv[i], "--face-node") == 0 && i + 1 < argc) o.faceNode = argv[++i];
        else if (strcmp(argv[i], "--gltf") == 0 && i + 1 < argc) o.gltfPath = argv[++i];
        else if (strcmp(argv[i], "--frametime") == 0) o.frametime = true;
        else if (strcmp(argv[i], "--fps-cap") == 0 && i + 1 < argc) o.fpsCap = atof(argv[++i]);
        else if (strcmp(argv[i], "--fps-jitter") == 0 && i + 1 < argc) o.fpsJitterMs = atof(argv[++i]);
        else if (strcmp(argv[i], "--fullscreen") == 0) o.fullscreenWindow = true;
        else if (strcmp(argv[i], "--dlaa") == 0) { gDlaa = true; kRenderW = kOutW; kRenderH = kOutH; }
        else if (strcmp(argv[i], "--yaw-speed") == 0 && i + 1 < argc) g_yawSpeed = (float) atof(argv[++i]);
        else if (strcmp(argv[i], "--moving-boxes") == 0) o.movingBoxes = true;
        else if (strcmp(argv[i], "--reference") == 0) o.reference = true;
        else if (strcmp(argv[i], "--hdri") == 0 && i + 1 < argc) o.hdriPath = argv[++i];
        else if (strcmp(argv[i], "--hdri-yaw") == 0 && i + 1 < argc) { const char *v = argv[++i]; if (strcmp(v, "auto") == 0) o.hdriYawAuto = true; else { o.hdriYawAuto = false; o.hdriYaw = (float) atof(v); } }
        else if (strcmp(argv[i], "--sun-scale") == 0 && i + 1 < argc) o.sunScale = (float) atof(argv[++i]);
        else if (strcmp(argv[i], "--exposure") == 0 && i + 1 < argc) o.exposure = (float) atof(argv[++i]);
        else if (strcmp(argv[i], "--shadow-soft") == 0 && i + 1 < argc) o.shadowSoft = (float) atof(argv[++i]);
        else if (strcmp(argv[i], "--anim-speed") == 0 && i + 1 < argc) o.animSpeed = (float) atof(argv[++i]);
        else if (strcmp(argv[i], "--hdri-mirror") == 0 && i + 1 < argc) { o.hdriMirrorArg = atoi(argv[++i]); }
        else if (strcmp(argv[i], "--sun-dir") == 0 && i + 1 < argc) o.sunDirGiven = parseVec3(argv[++i], o.sunDirTravel);
        else if (strcmp(argv[i], "--sun-strength") == 0 && i + 1 < argc) { o.sunStrength = (float) atof(argv[++i]); o.sunStrengthGiven = true; }
        else if (strcmp(argv[i], "--cam-pos") == 0 && i + 1 < argc) o.camPosGiven = parseVec3(argv[++i], o.camPosM);
        else if (strcmp(argv[i], "--cam-target") == 0 && i + 1 < argc) o.camTargetGiven = parseVec3(argv[++i], o.camTargetM);
        else if (strcmp(argv[i], "--fov") == 0 && i + 1 < argc) { o.camFov = (float) atof(argv[++i]); o.fovGiven = true; }
        else if (strcmp(argv[i], "--cam-shift") == 0 && i + 1 < argc) { float a = 0, b = 0; if (sscanf_s(argv[++i], "%f,%f", &a, &b) == 2) { o.camShift = {a, b, 0}; o.camShiftGiven = true; } }
        else if (strcmp(argv[i], "--scene-scale") == 0 && i + 1 < argc) { o.sceneScale = (float) atof(argv[++i]); o.sceneScaleGiven = true; }
        else if (strcmp(argv[i], "--scene-yaw") == 0 && i + 1 < argc) { o.sceneYaw = (float) atof(argv[++i]); o.sceneYawGiven = true; }
        else o.frames = atoi(argv[i]);
    }
    // ---- --camera blender: the user's Blender setup for the cutegirl head, baked in as defaults ----
    // Nothing is read from disk; every constant below is still overridable by its own flag. The
    // numbers come from the .blend the look was tuned in (glTF Y-up, metres, "Standard" view
    // transform, HDRI strength 1, 0 EV, 57 mm lens on a 3840x2160 frame).
    const bool blenderCam = strcmp(o.cameraMode, "blender") == 0;
    if (blenderCam) {
        if (!o.sunDirGiven) { o.sunDirTravel = {0.522451f, -0.654712f, 0.546258f}; o.sunDirGiven = true; }
        if (!o.sunStrengthGiven) { o.sunStrength = 3.01f; o.sunStrengthGiven = true; }
        if (!o.camPosGiven) { o.camPosM = {-0.19384f, 1.487888f, -0.669449f}; o.camPosGiven = true; }
        if (!o.camTargetGiven) { o.camTargetM = {-0.004746f, 1.552911f, -0.010353f}; o.camTargetGiven = true; }
        if (!o.fovGiven) { o.camFov = 23.7773f; o.fovGiven = true; }
        if (!o.camShiftGiven) { o.camShift = {-0.08f, -0.09f, 0.0f}; o.camShiftGiven = true; } // the .blend's camera lens shift_x / shift_y, Blender units (fraction of the frame WIDTH)

        if (!o.sceneScaleGiven) { o.sceneScale = 100.0f; o.sceneScaleGiven = true; } // the cutegirl .glb is authored in centimetres
        // The bench's world is the glTF world with Z negated (the head faces +Z here and -Z in Blender's
        // Y-up export), so everything authored in the .blend is MIRRORED in Z, not turned: a 180 deg
        // yaw gave the mirrored head turn. Verified against the user's Blender render (2026-09-08).
        if (!o.sceneYawGiven) { o.sceneYaw = 0.0f; o.sceneYawGiven = true; }
        o.camPosM.z = -o.camPosM.z; o.camTargetM.z = -o.camTargetM.z; o.sunDirTravel.z = -o.sunDirTravel.z; // the lamp mirrors with the camera (the user confirmed: key from the viewer's right)
        if (o.hdriMirrorArg < 0) o.hdriMirrorArg = 1; // the environment is mirrored with the world
        // Blender's equirect world is ours turned a quarter turn: ourYaw = -90 deg - blenderYaw
        // (measured to 0.4 deg, see the note at --hdri-yaw). blenderYaw is 0 in this .blend.
        if (o.hdriYawAuto) { o.hdriYaw = -90.0f; o.hdriYawAuto = false; } // with the Z mirror: matched to the Blender render's background (hills far left, ridge right)
        std::printf("[info] camera blender: pos (%.5f, %.5f, %.5f) target (%.5f, %.5f, %.5f) fovY %.4f deg, scene scale %.0f\n",
                    o.camPosM.x, o.camPosM.y, o.camPosM.z, o.camTargetM.x, o.camTargetM.y, o.camTargetM.z, o.camFov, o.sceneScale);
    }
    // --scene-yaw: one rotation about +Y applied to everything that was authored in the other scene -
    // the camera override, the sun direction and the environment (the sky's yaw simply adds).
    if (o.sceneYaw != 0.0f) {
        const float a = o.sceneYaw * 3.14159265f / 180.0f, c = std::cos(a), s = std::sin(a);
        auto rotY = [&](Vec3 v) { return Vec3{v.x * c + v.z * s, v.y, -v.x * s + v.z * c}; };
        if (o.camPosGiven) o.camPosM = rotY(o.camPosM);
        if (o.camTargetGiven) o.camTargetM = rotY(o.camTargetM);
        if (o.sunDirGiven) o.sunDirTravel = rotY(o.sunDirTravel);
        if (!o.hdriYawAuto) o.hdriYaw += o.sceneYaw; // "auto" picks it up through faceYaw / the else branch below
        o.faceYaw += o.sceneYaw; // the face camera orbits the asset, so it follows the same turn
        std::printf("[info] scene yaw %.1f deg: camera (%.5f, %.5f, %.5f) -> (%.5f, %.5f, %.5f), sun travel (%.4f, %.4f, %.4f), hdri yaw %.1f\n",
                    o.sceneYaw, o.camPosM.x, o.camPosM.y, o.camPosM.z, o.camTargetM.x, o.camTargetM.y, o.camTargetM.z,
                    o.sunDirTravel.x, o.sunDirTravel.y, o.sunDirTravel.z, o.hdriYaw);
    }
    g_hdriMirror = o.hdriMirrorArg > 0;
    for (size_t k = 0; k < o.nrPause.size(); k += 2)
        if (o.nrPause.size() % 2 != 0 || o.nrPause[k] >= o.nrPause[k + 1] || (k > 0 && o.nrPause[k] < o.nrPause[k - 1])) {
            std::printf("[fail] --nr-pause expects A,B[,A,B...] ascending\n"); return false;
        }
    if (o.mvScale == 0.0f) o.mvScale = 1.0f;
    o.flat = strcmp(o.scene, "flat") == 0;
    if (o.reference && o.flat) { std::printf("[fail] --reference needs the 3D scene\n"); return false; }
    return true;
}

bool NrPaused(const BenchOptions &o, int frame)
{
    for (size_t k = 0; k + 1 < o.nrPause.size(); k += 2) if (frame >= o.nrPause[k] && frame < o.nrPause[k + 1]) return true;
    return false;
}

void PrintSceneSummary(const BenchOptions &o)
{
    std::printf("[info] scene %s, camera %s, exposure %.2f, shadow-soft %.4f, DLSS MV_Scale = %.3f, mv-dir %+.0f%s%s\n", o.flat ? "flat" : "3d", o.cameraMode,
                o.exposure, o.shadowSoft, o.mvScale, o.mvDir,
                o.movingBoxes ? ", moving boxes" : "", o.reference ? ", REFERENCE (no DLSS/NR, 8 jitter samples at 4K)" : "");
}
