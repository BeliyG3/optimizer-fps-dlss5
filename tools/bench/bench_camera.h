// The bench camera: the scripted third-person path and the per-frame choice between it, the glTF
// file's camera, the face close-up and the fixed --cam-pos override.
#pragma once
#include "bench_math.h"

struct BenchOptions;
namespace pwgltf { struct Scene; }

inline float g_yawSpeed = 45.0f; // --yaw-speed, degrees per second

// The camera path: third person around a figure at the origin. Phases of 120 frames.
struct CameraState { Vec3 eye, target; };
CameraState CameraAt(int frame, const char *mode);

// The camera of this frame (with --cam-dolly, --cam-lift and --sway applied) and its vertical FOV.
CameraState ChooseCamera(const BenchOptions &o, const pwgltf::Scene &gscene, int frame, float &fovY);
