// Rendering one frame: the 3D scene (bench_render.cpp), the flat scene, the --reference
// accumulation and the blit to the back buffer (bench_frame.cpp).
#pragma once
#include "bench_d3d.h"

struct BenchOptions;
struct BenchDevice;
struct BenchScene;
struct GltfScene;

// Jitter sequence (8 samples, the same for all frames of an 8-cycle).
void Jitter(int index, float *jx, float *jy);
// Renders the 3D scene once into colour/motion/depth with the given jitter (and weight for the
// reference accumulation).
void RenderScene3D(const BenchOptions &o, BenchDevice &d, BenchScene &s, GltfScene &gl, int frame, float jx, float jy, float weight, bool additiveColor, UINT w, UINT h);
// The historical moving checkerboard at the render size.
void RenderFlat(const BenchOptions &o, BenchDevice &d, BenchScene &s, int frame);
// --reference: jittered 4K renders of the 3D scene averaged into the output (no DLSS/NR).
void RenderReference(const BenchOptions &o, BenchDevice &d, BenchScene &scene, GltfScene &gl, int frame);
// The DLSS output (or, while --nr-pause holds, the render-size colour) stretched onto the back buffer.
void PresentBlit(const BenchOptions &o, BenchDevice &d, BenchScene &s, bool paused);
