// pw_bench command line: every option with its default (the usage list is in bench_options.cpp).
#pragma once
#include "bench_d3d.h"
#include "bench_math.h"

#include <vector>

inline const UINT kOutW = 3840, kOutH = 2160;
// --dlaa sets the render size to the output size (DLAA): the upscaler guides then match the colour, as in
// games that run DLSS at native resolution, which the external frame-generation export requires.
inline UINT kRenderW = 2258, kRenderH = 1270;
inline bool gDlaa = false;

struct BenchOptions {
    int frames = 600, switchEvery = 0;
    int temporalMode = -1, temporalEvery = 2;
    std::vector<int> dumpFrames;
    std::vector<int> nrPause; // --nr-pause: pairs A,B (half-open)
    bool dumpBack = false, debugLayer = false;
    float mvScale = 1.0f;
    float mvDir = 1.0f;
    int measureEvery = 0;
    UINT presentW = kOutW, presentH = kOutH;
    const char *scene = "3d";
    const char *cameraMode = "script";
    bool movingBoxes = false;
    bool frametime = false; // --frametime: statistics of the intervals between Present calls
    double fpsCap = 0.0;    // --fps-cap N: CPU-paced frame budget (emulates a game with idle GPU time)
    double fpsJitterMs = 0.0; // --fps-jitter MS: adds a uniform random 0..MS ms to each capped frame (uneven game pacing)
    bool fullscreenWindow = false; // --fullscreen: borderless popup covering the primary monitor (independent flip for an external presenter)
    const char *gltfPath = nullptr; // --gltf <file.glb>: replaces the procedural scene (camera from the file unless --camera is given)
    bool cameraGiven = false;
    float albedoScale = 1.0f;  // --albedo F: multiplier on the base colour of every glTF surface (not the lamps)
    float ambientScale = 1.0f; // --ambient F: multiplier on the environment's diffuse term only (the lamps keep their power)
    bool hdrOutput = false;  // --hdr: the scene colour is linear radiance (no tone mapper), as a game hands it to the upscaler
    float boil = 0.0f;       // --boil F: frame-to-frame noise in the dim parts of the frame (0.1 = +-10 %)
    float sway = 0.0f;       // --sway M: a third-person camera's idle sway, metres of amplitude at the eye
    float camDolly = 0.0f; // --cam-dolly: the chosen camera moved this far towards its target
    float camLift = 0.0f;  // --cam-lift: eye and target raised together (a dollied-in shot cuts the head off)
    float faceYaw = 180.0f, faceRadius = 0.0f, faceSweep = 25.0f; // --camera face (180: in front of a character that faces glTF -Z); radius 0 = auto (see kFaceFill)
    float faceAngle = -15.0f;       // --face-angle: the camera's horizontal offset from the face axis, so the head reads slightly turned
                                    // (negative: the head reads turned right-to-left, the mirror of the old +15)
    const char *faceNode = "CharacterAnchor";
    bool reference = false;
    const char *hdriPath = nullptr; // --hdri <file.hdr|none>; empty = look for the default beside the assets
    // --hdri-yaw: environment rotation about the vertical axis; "auto" (the default) aims the sun at
    // kSunOffCameraDeg to the side of the face camera and resolves to 0 for every other camera.
    float hdriYaw = 0.0f; bool hdriYawAuto = true;
    float sunScale = -1.0f;         // --sun-scale: overrides kSunOverSky (negative = use the constant)
    float exposure = 1.45f;         // --exposure: plain multiplier on the scene radiance (sky, ambient and sun alike) before the tone mapper
                                    // (1.45 with the soft kSunOverSky key: lit skin lands near sRGB luma 209, shadowed near 143, the sky unclipped at 203)
    int hdriMirrorArg = -1;          // --hdri-mirror 0|1 (-1 = preset decides)
    float animSpeed = 1.0f;         // --anim-speed: glTF animation playback factor (2.5 = the cutegirl clip plays 2.5x faster)
    float shadowSoft = 0.006f;      // --shadow-soft: penumbra half-width as a fraction of the shadow frustum's width
    // ---- Blender parity overrides (see --camera blender). Unset = NaN-free sentinels below. ----
    bool sunDirGiven = false, sunStrengthGiven = false, camPosGiven = false, camTargetGiven = false, fovGiven = false, sceneScaleGiven = false, sceneYawGiven = false, camShiftGiven = false;
    Vec3 sunDirTravel{0, -1, 0};    // --sun-dir: the direction the light TRAVELS (Blender's convention); lightDir = -this
    float sunStrength = 0.0f;       // --sun-strength: sun irradiance in the HDRI's radiance units (Blender's W/m^2 with HDRI strength 1)
    Vec3 camPosM{0, 0, 0}, camTargetM{0, 0, 0}; // --cam-pos / --cam-target, in the units the user typed (metres); x sceneScale -> scene units
    float camFov = 0.0f;            // --fov: vertical FOV in degrees
    Vec3 camShift{0, 0, 0};         // --cam-shift: Blender's camera shift_x / shift_y (z unused)
    float sceneScale = 1.0f;        // --scene-scale: multiplies the camera override positions (cutegirl is authored in cm: 100)
    float sceneYaw = 0.0f;          // --scene-yaw: the loaded asset is turned this many degrees about +Y relative to the
                                    // scene the camera/sun/HDRI numbers were authored in; camera, sun and sky are turned with it
    bool flat = false;              // --scene flat
};

// Parses the arguments, applies the --camera blender preset and --scene-yaw, and checks the
// combinations; false (after printing why) when the bench cannot run with them.
bool ParseOptions(int argc, char **argv, BenchOptions &o);
// --nr-pause: true for a frame inside one of the half-open A..B ranges.
bool NrPaused(const BenchOptions &o, int frame);
void PrintSceneSummary(const BenchOptions &o);
