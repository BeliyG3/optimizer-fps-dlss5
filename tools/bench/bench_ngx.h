// Native DLSS through the driver's NGX core (_nvngx.dll from the driver store), as a game calls it.
#pragma once
#include "bench_d3d.h"

#include "nvsdk_ngx.h"

struct BenchOptions;
struct BenchDevice;

using PFN_Evaluate = NVSDK_NGX_Result(NVSDK_CONV *)(ID3D11DeviceContext *, const NVSDK_NGX_Handle *, NVSDK_NGX_Parameter *, PFN_NVSDK_NGX_ProgressCallback);

struct DlssFeature {
    HMODULE ngx = nullptr; PFN_Evaluate evaluate = nullptr; NVSDK_NGX_Parameter *params = nullptr; NVSDK_NGX_Handle *feature = nullptr;
};

// NGX init (application data in exeDir) and the super-sampling feature; false after printing the failure.
bool CreateDlss(const wchar_t *exeDir, BenchDevice &d, DlssFeature &f);
// One DLSS evaluate of the render-size colour, depth and motion into the 4K output.
void EvaluateDlss(const BenchOptions &o, BenchDevice &d, DlssFeature &f, int frame, float jx, float jy);
