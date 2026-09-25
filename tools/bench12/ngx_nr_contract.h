#pragma once
#include "ngx_sdk.h"
#include <cstdio>

// DLSS Neural Rendering (NGX feature 18) parameter contract, as read by nvngx_dlssnr.dll 310.8.
// Types follow the runtime's own getters: sizes/masks/Style unsigned, sub-rects and flags int,
// strengths/scales float. See NGX_PARAMETERS.md for the probe results behind every value.
constexpr NVSDK_NGX_Feature NgxFeatureNeuralRendering=static_cast<NVSDK_NGX_Feature>(18);

struct NrRect { unsigned x=0, y=0, width=0, height=0; };
struct NrCreateInfo {
    unsigned width=0, height=0;   // DLSSNR.Width/Height: the size the network is built for
    int perfQuality=-1;           // PerfQualityValue, written when >= 0 (the runtime's create ignores it)
    float scalingRatio=0;         // DLSSNR.ScalingRatio, written when > 0 (310.8 reads it and keeps scale 1)
    unsigned preset=1;            // 310.8 ships preset 1 only; 0 falls back to it with a log line
};
struct NrFrame {
    ID3D12Resource *colour=nullptr, *depth=nullptr, *motion=nullptr, *output=nullptr;
    NrRect colourRect, guideRect, outputRect;
    float mvScaleX=1, mvScaleY=1, scalingRatio=0;
    bool depthInverted=false, reset=false;
};
// Strengths match the renodx-dlss5 defaults of the add-on test runtime (NRIntensity/LocalTone/
// LocalStructure/SkinStructure = 2, Style 0, no auto mask, no UI correction).
constexpr float NrStrength=2.0f;

inline void NrWriteRect(NVSDK_NGX_Parameter &p, const char *name, const NrRect &r)
{
    char key[64];
    std::snprintf(key,sizeof(key),"DLSSNR.%sSubrectBaseX",name); p.Set(key,int(r.x));
    std::snprintf(key,sizeof(key),"DLSSNR.%sSubrectBaseY",name); p.Set(key,int(r.y));
    std::snprintf(key,sizeof(key),"DLSSNR.%sSubrectWidth",name); p.Set(key,int(r.width));
    std::snprintf(key,sizeof(key),"DLSSNR.%sSubrectHeight",name); p.Set(key,int(r.height));
}
inline void NrWriteCreate(NVSDK_NGX_Parameter &p, const NrCreateInfo &c)
{
    p.Set("CreationNodeMask",1u); p.Set("VisibilityNodeMask",1u);
    p.Set("DLSSNR.Width",c.width); p.Set("DLSSNR.Height",c.height);
    p.Set("DLSSNR.Hint.Render.Preset",int(c.preset));
    if(c.perfQuality>=0) p.Set("PerfQualityValue",c.perfQuality);
    if(c.scalingRatio>0) p.Set("DLSSNR.ScalingRatio",c.scalingRatio);
}
inline void NrWriteEvaluate(NVSDK_NGX_Parameter &p, const NrFrame &f)
{
    p.Set("DLSSNR.Color",f.colour); p.Set("DLSSNR.Depth",f.depth);
    p.Set("DLSSNR.MVec",f.motion); p.Set("DLSSNR.Output",f.output);
    // The sub-rects are mandatory: without them the runtime sees 0x0 rects and skips the evaluate.
    NrWriteRect(p,"Color",f.colourRect); NrWriteRect(p,"Depth",f.guideRect);
    NrWriteRect(p,"MVec",f.guideRect); NrWriteRect(p,"Output",f.outputRect);
    p.Set("DLSSNR.MVecScaleX",f.mvScaleX); p.Set("DLSSNR.MVecScaleY",f.mvScaleY);
    if(f.scalingRatio>0) p.Set("DLSSNR.ScalingRatio",f.scalingRatio);
    p.Set("DLSSNR.DepthInverted",f.depthInverted ? 1 : 0); p.Set("DLSSNR.Reset",f.reset ? 1 : 0);
    p.Set("DLSSNR.Enabled",1); p.Set("DLSSNR.Style",0u);
    p.Set("DLSSNR.Intensity",NrStrength); p.Set("DLSSNR.LocalToneStrength",NrStrength);
    p.Set("DLSSNR.LocalStructureStrength",NrStrength); p.Set("DLSSNR.SkinStructureStrength",NrStrength);
    p.Set("DLSSNR.UseAutoMask",0); p.Set("DLSSNR.UICorrection",0);
}
