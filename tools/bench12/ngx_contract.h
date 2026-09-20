#pragma once
#include "camera.h"
#include "ngx_sdk.h"
#include <array>

inline NVSDK_NGX_PerfQuality_Value NgxQuality(float scale)
{
    // Nearest standard ratio; exact render dimensions are always passed separately.
    const float ratios[]={1.0f/3,0.5f,0.58f,2.0f/3,1};
    const NVSDK_NGX_PerfQuality_Value modes[]={NVSDK_NGX_PerfQuality_Value_UltraPerformance,
        NVSDK_NGX_PerfQuality_Value_MaxPerf,NVSDK_NGX_PerfQuality_Value_Balanced,
        NVSDK_NGX_PerfQuality_Value_MaxQuality,NVSDK_NGX_PerfQuality_Value_DLAA};
    unsigned best=0;
    for(unsigned i=1;i<5;++i) if(std::abs(scale-ratios[i])<std::abs(scale-ratios[best])) best=i;
    return modes[best];
}
inline int NgxFlags(bool reverse)
{
    return NVSDK_NGX_DLSS_Feature_Flags_IsHDR|NVSDK_NGX_DLSS_Feature_Flags_MVLowRes|
        (reverse ? NVSDK_NGX_DLSS_Feature_Flags_DepthInverted : 0);
}
inline Mat4 NgxMatrix(const Mat4 &m)
{
    // NGX expects row-major storage AND row vectors (v*M). The renderer uses M*v.
    Mat4 result;
    for(int row=0;row<4;++row) for(int col=0;col<4;++col) result.m[row][col]=m.m[col][row];
    return result;
}
struct NgxFrame {
    std::array<ID3D12Resource *,7> inputs{}; // colour, depth, motion, normal/roughness, diffuse, specular, hit distance
    ID3D12Resource *output=nullptr;
    unsigned width=0, height=0;
    float jitterX=0, jitterY=0, mvScaleX=1, mvScaleY=1;
    float deltaMilliseconds=1000.0f/60;
    bool reset=false;
    Mat4 worldToView{}, viewToClip{}; // Already converted to NGX row-vector convention.
};
inline NVSDK_NGX_DLSSD_Create_Params RrCreate(unsigned width, unsigned height, unsigned outWidth, unsigned outHeight, const Options &o)
{
    NVSDK_NGX_DLSSD_Create_Params p{};
    p.InWidth=width; p.InHeight=height; p.InTargetWidth=outWidth; p.InTargetHeight=outHeight;
    p.InPerfQualityValue=NgxQuality(o.renderScale); p.InFeatureCreateFlags=NgxFlags(o.reverse);
    p.InDenoiseMode=NVSDK_NGX_DLSS_Denoise_Mode_DLUnified;
    p.InRoughnessMode=NVSDK_NGX_DLSS_Roughness_Mode_Packed;
    p.InUseHWDepth=o.linearDepth ? NVSDK_NGX_DLSS_Depth_Type_Linear : NVSDK_NGX_DLSS_Depth_Type_HW;
    return p;
}
inline NVSDK_NGX_D3D12_DLSSD_Eval_Params RrEvaluate(NgxFrame &f)
{
    NVSDK_NGX_D3D12_DLSSD_Eval_Params p{};
    p.pInColor=f.inputs[0]; p.pInDepth=f.inputs[1]; p.pInMotionVectors=f.inputs[2]; p.pInOutput=f.output;
    p.pInNormals=f.inputs[3]; p.pInDiffuseAlbedo=f.inputs[4]; p.pInSpecularAlbedo=f.inputs[5];
    p.pInSpecularHitDistance=f.inputs[6]; // pInRoughness stays null: packed normals.w.
    p.InJitterOffsetX=f.jitterX; p.InJitterOffsetY=f.jitterY; p.InMVScaleX=f.mvScaleX; p.InMVScaleY=f.mvScaleY;
    p.InRenderSubrectDimensions={f.width,f.height}; p.InReset=f.reset ? 1 : 0;
    p.pInWorldToViewMatrix=&f.worldToView.m[0][0]; p.pInViewToClipMatrix=&f.viewToClip.m[0][0];
    p.InFrameTimeDeltaInMsec=f.deltaMilliseconds; p.InPreExposure=1; p.InExposureScale=1;
    return p;
}
