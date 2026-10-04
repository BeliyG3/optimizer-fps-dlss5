// Reading frames back: --measure (mean luminance of the DLSS output), --dump (the output as BMP)
// and --dump-back (the back buffer just before Present).
#pragma once
#include "bench_d3d.h"

struct BenchOptions;
struct BenchDevice;

struct Capture {
    ComPtr<ID3D11Texture2D> staging;     // the DLSS output's copy (--measure / --dump)
    ComPtr<ID3D11Texture2D> backStaging; // the back buffer's copy (--dump-back)
    double sumAll = 0, sumCenter = 0, sumPeriphery = 0; int measurements = 0;
};

// The staging textures the options ask for; false after printing the failure.
bool CreateCaptureTargets(const BenchOptions &o, BenchDevice &d, Capture &c);
void MeasureOutput(BenchDevice &d, Capture &c, int frameIndex);
void DumpOutput(BenchDevice &d, Capture &c, int frameIndex);
void DumpBackBuffer(const BenchOptions &o, BenchDevice &d, Capture &c, int frameIndex);
void PrintMeasureAverage(const Capture &c);
