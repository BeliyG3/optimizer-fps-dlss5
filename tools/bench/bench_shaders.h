// HLSL of the bench scenes (compiled at start-up) and the compile helper.
#pragma once
#include "bench_d3d.h"

#include <d3dcommon.h>

extern const char *const kShaderFlat; // the historical moving checkerboard (--scene flat)
extern const char *const kShader3D;   // ground, boxes, glTF meshes, sky, shadow pass, blit and accumulation

// Prints the compiler's message and returns null when the entry point does not compile.
ComPtr<ID3DBlob> Compile(const char *source, const char *entry, const char *target);
