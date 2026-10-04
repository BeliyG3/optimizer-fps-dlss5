// Radiance (.hdr) images and the equirect mapping the sky, the ambient lookup and the sun search share.
#pragma once
#include "bench_math.h"

#include <vector>

// Radiance (.hdr) RGBE image: header lines, "-Y h +X w", then new-style RLE scanlines (marker
// 2 2 hi lo, four run-length encoded channel planes) with the flat non-RLE layout as fallback.
// Row 0 is the top of the image (-Y), matching v = 0 = straight up in the equirect mapping.
struct HdrImage { int w = 0, h = 0; std::vector<float> rgb; }; // 3 floats per pixel, row-major from the top

bool LoadHdr(const char *path, HdrImage &img);

// --hdri-yaw: the environment is turned about the vertical axis in exactly one place, the equirect
// mapping below (mirrored by EquirectUV in HLSL, which gets cos/sun through env.xy): a world
// direction is rotated by -yaw into HDRI space, so the map appears turned by +yaw. Everything that
// comes *out* of the map in HDRI space - only the extracted sun direction - is turned by +yaw with
// RotateHdriYaw, so the shadows stay consistent with the sky.
inline float g_hdriYawCos = 1.0f, g_hdriYawSin = 0.0f;
inline float g_hdriMeanL = 1.0f; // solid-angle weighted mean radiance (luma) of the loaded HDRI
inline bool g_hdriMirror = false; // --hdri-mirror: u -> 1 - u (the bench world is the glTF/Blender world mirrored in Z)
inline Vec3 RotateHdriYaw(Vec3 m) { return {m.x * g_hdriYawCos + m.z * g_hdriYawSin, m.y, -m.x * g_hdriYawSin + m.z * g_hdriYawCos}; } // HDRI space -> world

// The one equirect mapping used by the sky, the ambient lookup and the sun search (mirrored in HLSL).
void HdrDirToUv(Vec3 w, float &u, float &v);
// The plain inverse, in HDRI space: the sun search works on the map and rotates its result once.
Vec3 HdrUvToDir(float u, float v);

unsigned short FloatToHalf(float f);
