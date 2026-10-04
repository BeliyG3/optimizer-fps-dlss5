#include "bench_ngx.h"

#include "bench_device.h"
#include "bench_options.h"

#include <cstdio>

using PFN_Init = NVSDK_NGX_Result(NVSDK_CONV *)(unsigned long long, const wchar_t *, ID3D11Device *, const NVSDK_NGX_FeatureCommonInfo *, NVSDK_NGX_Version);
using PFN_Alloc = NVSDK_NGX_Result(NVSDK_CONV *)(NVSDK_NGX_Parameter **);
using PFN_Create = NVSDK_NGX_Result(NVSDK_CONV *)(ID3D11DeviceContext *, NVSDK_NGX_Feature, NVSDK_NGX_Parameter *, NVSDK_NGX_Handle **);
using PFN_Release = NVSDK_NGX_Result(NVSDK_CONV *)(NVSDK_NGX_Handle *);
using PFN_Shutdown1 = NVSDK_NGX_Result(NVSDK_CONV *)(ID3D11Device *);

bool CreateDlss(const wchar_t *exeDir, BenchDevice &d, DlssFeature &f)
{
    f.ngx = LoadLibraryW(L"C:\\WINDOWS\\System32\\DriverStore\\FileRepository\\nv_dispi.inf_amd64_a3944b54ff18b284\\_nvngx.dll");
    if (!f.ngx) { std::printf("[fail] _nvngx.dll\n"); return false; }
    auto ngxInit = (PFN_Init) GetProcAddress(f.ngx, "NVSDK_NGX_D3D11_Init");
    auto ngxAlloc = (PFN_Alloc) GetProcAddress(f.ngx, "NVSDK_NGX_D3D11_AllocateParameters");
    auto ngxCreate = (PFN_Create) GetProcAddress(f.ngx, "NVSDK_NGX_D3D11_CreateFeature");
    f.evaluate = (PFN_Evaluate) GetProcAddress(f.ngx, "NVSDK_NGX_D3D11_EvaluateFeature");
    if (!ngxInit || !ngxAlloc || !ngxCreate || !f.evaluate) { std::printf("[fail] NGX exports\n"); return false; }
    NVSDK_NGX_Result r = ngxInit(0x24480451ull, exeDir, d.dev.Get(), nullptr, NVSDK_NGX_Version_API);
    std::printf("[info] NGX D3D11 init -> 0x%08X\n", (unsigned) r);
    if (NVSDK_NGX_FAILED(r)) return false;
    r = ngxAlloc(&f.params);
    if (NVSDK_NGX_FAILED(r) || !f.params) { std::printf("[fail] params 0x%08X\n", (unsigned) r); return false; }
    f.params->Set(NVSDK_NGX_Parameter_Width, kRenderW);
    f.params->Set(NVSDK_NGX_Parameter_Height, kRenderH);
    f.params->Set(NVSDK_NGX_Parameter_OutWidth, kOutW);
    f.params->Set(NVSDK_NGX_Parameter_OutHeight, kOutH);
    f.params->Set(NVSDK_NGX_Parameter_PerfQualityValue, (int) (gDlaa ? NVSDK_NGX_PerfQuality_Value_DLAA : NVSDK_NGX_PerfQuality_Value_Balanced));
    f.params->Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, (int) (NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes | (g_standardDepth ? 0 : NVSDK_NGX_DLSS_Feature_Flags_DepthInverted) | NVSDK_NGX_DLSS_Feature_Flags_DoSharpening | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure));
    f.params->Set(NVSDK_NGX_Parameter_CreationNodeMask, 1u);
    f.params->Set(NVSDK_NGX_Parameter_VisibilityNodeMask, 1u);
    r = ngxCreate(d.ctx.Get(), NVSDK_NGX_Feature_SuperSampling, f.params, &f.feature);
    std::printf("[info] DLSS create -> 0x%08X\n", (unsigned) r);
    if (NVSDK_NGX_FAILED(r)) return false;
    return true;
}

void EvaluateDlss(const BenchOptions &o, BenchDevice &d, DlssFeature &f, int frame, float jx, float jy)
{
    f.params->Set(NVSDK_NGX_Parameter_Color, (ID3D11Resource *) d.color.Get());
    f.params->Set(NVSDK_NGX_Parameter_Depth, (ID3D11Resource *) d.depth.Get());
    f.params->Set(NVSDK_NGX_Parameter_MotionVectors, (ID3D11Resource *) d.motion.Get());
    f.params->Set(NVSDK_NGX_Parameter_Output, (ID3D11Resource *) d.output.Get());
    f.params->Set(NVSDK_NGX_Parameter_Jitter_Offset_X, jx);
    f.params->Set(NVSDK_NGX_Parameter_Jitter_Offset_Y, jy);
    f.params->Set(NVSDK_NGX_Parameter_MV_Scale_X, o.mvScale);
    f.params->Set(NVSDK_NGX_Parameter_MV_Scale_Y, o.mvScale);
    f.params->Set(NVSDK_NGX_Parameter_Sharpness, 0.15f);
    f.params->Set(NVSDK_NGX_Parameter_Reset, frame == 0 ? 1 : 0);
    f.params->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, kRenderW);
    f.params->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, kRenderH);
    f.params->Set(NVSDK_NGX_Parameter_DLSS_Pre_Exposure, 1.0f);
    f.params->Set(NVSDK_NGX_Parameter_DLSS_Exposure_Scale, 1.0f);
    const NVSDK_NGX_Result r = f.evaluate(d.ctx.Get(), f.feature, f.params, nullptr);
    if (NVSDK_NGX_FAILED(r)) std::printf("[warn] frame %d evaluate 0x%08X\n", frame, (unsigned) r);
}
