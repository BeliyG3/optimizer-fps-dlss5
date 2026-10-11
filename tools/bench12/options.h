#pragma once
#include "math.h"
#include <string>
#include <vector>
#include <stdexcept>
#include <cstdio>
#include <climits>

// A rejected --motion-span: main exits with code 2 (a ground-truth run that cannot be exact).
struct MotionSpanError : std::runtime_error { using std::runtime_error::runtime_error; };
struct Options {
    int frames=600, width=1920, height=1080, spp=1, bounces=4, lightCandidates=8;
    float renderScale=0.5f, fov=60, yawSpeed=45, moveSpeed=2, sway=0, camDolly=0, camLift=0;
    float sunStrength=1, sunAngle=0.5f, exposure=1.45f, albedo=1, firefly=50;
    float haze=0.012f, hazeG=0.6f, bloom=0.06f;
    bool autoExposure=false;
    float exposureSpeedUp=3, exposureSpeedDown=1.5f; // --auto-exposure 1: stops' closing rate (1/s) towards a brighter / a darker scene
    float exposureEv=0, exposureCentre=0; // --auto-exposure 1: compensation in stops on the metered target; histogram centre weighting 0..1
    int linearDepth=0; // 0 hardware, -1 negative view Z, +1 positive view Z
    float animSpeed=1, animStart=0;
    bool animPlaying=true;
    int blasRebuild=60;
    std::string followNode;
    bool interactive=false, accumulationEnabled=false, accumulationInfinite=false, feedAccumulation=false;
    int accumulationFrames=256;
    std::string settings;
    std::string tonemap="aces"; // aces (the per-channel fit), neutral, none, ue-filmic (UE's filmic curve, tonemap_ue.hlsli)
    float filmSlope=0.88f, filmToe=0.55f, filmShoulder=0.26f, filmBlackClip=0, filmWhiteClip=0.04f; // ue-filmic: UE's "Film" defaults
    bool localExposure=false; // --local-exposure 1: UE5's local exposure (local_exposure.hlsli), contrast scales of the base
    float localHighlights=0.8f, localShadows=0.8f, localDetail=1, localBlend=0.6f;
    std::string exposureMetering="bench"; // --auto-exposure 1: bench (average, highlight cap) or ue (histogram 10..90 % mean to 18 %)
    float exposureMinEv=0.5f, exposureMaxEv=20; // ue metering: UE's Min/Max EV100 of the total exposure (MeterScaleBounds)
    // ue metering: UE's Exposure Compensation Curve as a ramp, x = the scene's metered EV100, y = extra stops: 0 at or below
    // x (low), z at or above y (high) (ExposureCurveBias, display_math.hlsli)
    Vec3 exposureCurve{3,6,0.33f};
    Vec3 sunDir{0,-1,0}, camPos{}, camTarget{};
    bool jitter=true, reverse=false, debug=false, cameraOverride=false, help=false, vsync=false;
    std::string upscaler="none", motion="pixels"; // motion: pixels|ndc layout, or none (zero vectors; the add-on's optical flow is then the source)
    std::string render="pathtrace"; // pathtrace, or raster: the lab drawn like a game, without ray tracing
    int renderSwitchAt=-1; // --render-switch-at F: frame F flips the render mode (raster <-> pathtrace), as a settings change would
    float rasterAmbient=0.165f; // --render raster: ambient term of the shading (tuned against path tracing, RASTER.md)
    bool rasterSkyAmbient=false; // --raster-sky-ambient: the raster ambient term is the --hdri sky's SH9 irradiance, not a constant (sky_sh.h)
    bool probes=false; float probeSpacing=1; int probeRays=256; std::string probeDump; // --probes: the raster ambient from traced irradiance probes (probes.h)
    bool lampTubes=false; // --lamp-tubes: raster draws lamp tubes as line lights, compact emitters stay point lights (lamp_parts.h)
    bool lampShadows=false; // --lamp-shadows: raster shadows each lamp group's light with a spot shadow map (raster_lamp_shadows.h)
    bool fxaa=false; // --fxaa: raster anti-aliases its finished frame with FXAA 3.11 in the resolve (shaders/raster_fxaa.hlsli)
    // --sky-reflections 0|1: raster adds the sky's image-based specular (RASTER_REFLECTIONS.md); -1 (the default): on with --hdri
    int skyReflections=-1;
    bool SkyReflectionsOn() const { return skyReflections<0 ? !hdri.empty() : skyReflections!=0; }
    float snapDeg=90; int snapFrames=6, snapHold=54; // --camera snap: turn angle, frames of the turn, frames held at each end
    bool wind=false; float windStrength=1; // --wind: veg_ nodes sway (wind.h); --wind-strength S scales the displacement
    bool hud=false; // --hud: a game-like HUD drawn into the frame
    bool gpuSkinning=true; // --cpu-skinning: the dynamic vertices are skinned by Animation's CPU workers, not the GPU pass (gpu_skinning.h)
    // Culling of small instances (raster_cull.h): --no-small-culling, --cull-small-radius R, --cull-cone-deg D, --cull-small-distance M;
    // --no-occlusion-culling (Hi-Z, raster_occlusion.h), --cull-check (draw everything, report culled-but-visible), --cull-debug (overlay).
    bool smallCulling=true, occlusionCulling=true, cullCheck=false, cullDebug=false;
    float cullSmallRadius=0.75f, cullConeDeg=170, cullSmallDistance=0;
    bool instancing=true; // --no-instancing: meshes shared by static nodes are flattened per node too (scene_instances.h)
    std::string lightmap; // --lightmap file.json: --render raster lights static geometry from baked lightmaps (lightmap.h)
    bool csm=false, csmStable=true; float csmDistance=100; // --csm: --render raster sun shadow in 4 cascades (raster_csm.h); snapped, far end of the last
    std::string ao; float aoRadius=1.0f, aoStrength=1.0f, aoAlpha=0.1f; // --ao clean|game: --render raster screen-space ambient occlusion; --ao-alpha: game's history blend
    std::string host="ngx";
    std::string coreStateSentinel;
    bool coreStateRebind=false;
    bool coreFlow=false; // --core-flow: the core's temporal motion source is NVIDIA optical flow (game frames)
    int coreMode=2, coreTemporal=0, coreWarpPath=0;
    std::string nr="off", mvFormat="rg16f"; // DLSS Neural Rendering host mode; motion texture layout for NGX
    std::string nrColour="srgb"; // colour handed to NR: sRGB-encoded proxy, or linear HDR as rendered
    int switchEvery=0; // --switch N: cycle the add-on's layout every N frames (bench of the retire path)
    std::string nrProxyFormat="rgba16f"; // format of the sRGB proxy handed to NR (r10g10b10a2: colour and output formats differ, as with DLSS5-Reshade-AIO)
    std::string nrList="direct"; // list NR is evaluated on: the frame's direct list, or a COMPUTE list on a compute queue (DLSS5-Reshade-AIO)
    int nrLog=1; // NR runtime file log level echoed as [nr log] (0 off, 1 on, 2 verbose)
    int nrPadX=0, nrPadY=0, nrBaseX=0, nrBaseY=0; // NR output texture padding and region base
    int nrColourPadX=0, nrColourPadY=0, nrColourPadEdge=0; // NR colour (sRGB proxy) texture padding (region at 0,0); 1 = pad repeats the edge
    std::vector<std::pair<int,int>> nrPause; // --nr-pause A,B[,A2,B2...]: half-open ranges, frames A<=f<B skip the feature-18 evaluate ("menu")
    std::string nrPauseShow="frozen"; // what a paused frame presents: the last NR output (frozen) or this frame's NR input (a menu without NR)
    int nrRecreate=-1; // --nr-recreate F: frame F releases feature 18 and creates it again (menu mode's lifetime check)
    std::vector<int> nrResetAt; // --nr-reset-at F[,F2...]: frame F's feature-18 evaluate reads DLSSNR.Reset=1 (a short run's first evaluate)
    struct SetAt { int frame=-1, id=0, value=0; };
    std::vector<SetAt> setAt; // --set-at F,ID,VALUE[,F2,ID2,VALUE2...]: frame F sets one add-on setting (menu mode's settings checks)
    bool nrReleaseThread=false; // --nr-release-thread: that release runs on a second thread during frame F's present (menu mode's C4 race)
    int resizeAt=-1, resizeWidth=0, resizeHeight=0; // --resize-at F[,W,H]: frame F resizes the swap chain (W,H 0: same size)
    int recreateSwapChainAt=-1; // --recreate-swapchain F: frame F destroys the swap chain and creates a new one on the window
    std::string gltf="../bench/assets/lab_scene.glb", hdri, camera="script", view="colour";
    std::vector<int> dumps;
    std::string dumpFormat="bmp"; // --dump-format bmp|png: dump_N.bmp (default) or dump_N.png (same pixels, lossless)
    std::string dumpRaw; // --dump-raw motion|depth|motion,depth|prevdepth: motion_N.f32 / depth_N.f32 / prevdepth_N.f32 (raw_dump.h) beside the exe for every --dump frame
    bool DumpsRaw(const char *what) const { return dumpRaw==what || ((dumpRaw=="motion,depth" || dumpRaw=="depth,motion") && std::string(what)!="prevdepth"); }
    int motionSpan=1; // --motion-span K: frame t's motion is the offset to frame t-K (clamped to 0), the ground truth of a chained offset
    bool motionSpanCadence=false; // --motion-span cadence: the span follows the full-pass cadence instead of a fixed K
    int cadence=0;      // --motion-span cadence --cadence N: frame t's motion is the offset to its last full pass (SpanAt)
    int cadencePhase=0; // --cadence-phase P: full passes are the frames with (t - P) mod N == 0
    int SpanAt(int frame) const {
        if (cadence<=0) return motionSpan;
        const int k=((frame-cadencePhase)%cadence+cadence)%cadence;
        return k==0 ? cadence : k; // a full pass needs the offset to the previous full pass, N back
    }
    int variants=1; // --variants K: the window [--start-frame, frames) K times in one process (application.cpp StartVariant)
    std::vector<SetAt> variantSet; // --variant-set K,ID,VALUE[,...]: before variant K, add-on setting ID = VALUE (frame holds K)
    std::string textureCache; // --texture-cache DIR: the scene images' mip chains on disk (texture_cache.h)
    bool startupProfile=false; // --startup-profile: the table of the run's phases at the end (startup_profile.h)
    int startFrame=0; // --start-frame S: the loop starts at frame S as if frames 0..S-1 had run (application.cpp)
    std::string exposureRecord, exposureReplay; // --exposure-record / --exposure-replay FILE: the auto exposure per frame (exposure_track.h)
    bool NrResetAt(int frame) const { return std::find(nrResetAt.begin(),nrResetAt.end(),frame)!=nrResetAt.end(); }
    bool NrPaused(int frame) const { for(auto &r:nrPause) if(frame>=r.first && frame<r.second) return true; return false; }
};
inline float ParseFloat(const std::string &s)
{
    size_t end=0; float n=std::stof(s,&end);
    if (end!=s.size() || !std::isfinite(n)) throw std::runtime_error("Invalid number: "+s);
    return n;
}
inline int ParseInt(const std::string &s)
{
    size_t end=0; long long n=std::stoll(s,&end);
    if (end!=s.size() || n<INT_MIN || n>INT_MAX) throw std::runtime_error("Invalid integer: "+s);
    return int(n);
}
inline Vec3 ParseVec(const std::string &s)
{
    size_t a=s.find(','), b=s.find(',',a==std::string::npos ? 0 : a+1);
    if (a==std::string::npos || b==std::string::npos) throw std::runtime_error("Expected x,y,z: "+s);
    return {ParseFloat(s.substr(0,a)),ParseFloat(s.substr(a+1,b-a-1)),ParseFloat(s.substr(b+1))};
}
inline Options ParseOptions(int argc, char **argv, Options o={})
{
    bool pos=o.cameraOverride, target=o.cameraOverride, frames=false, probeSet=false, windStrengthSet=false, csmStableSet=false, csmDistanceSet=false, aoRadiusSet=false, aoStrengthSet=false, aoAlphaSet=false, fxaaSet=false, cadenceSet=false;
    for (int i=1; i<argc; ++i) {
        std::string key=argv[i];
        auto value=[&]() -> std::string { if (++i>=argc) throw std::runtime_error("Missing value for "+key); return argv[i]; };
        if (key=="--sky-reflections") { // outside the else-if chain below, which is at MSVC's nesting limit (C1061)
            o.skyReflections=ParseInt(value()); // -1 (auto) only comes from a settings file saved before it was toggled
            if(o.skyReflections<-1 || o.skyReflections>1) throw std::runtime_error("--sky-reflections requires 0 or 1");
            continue;
        }
        if (key=="--motion-span" || key=="--dump-raw" || key=="--cadence" || key=="--cadence-phase") { // outside the else-if chain too (C1061)
            if (key=="--dump-raw") { o.dumpRaw=value(); if (!o.DumpsRaw("motion") && !o.DumpsRaw("depth") && !o.DumpsRaw("prevdepth")) throw std::runtime_error("Invalid --dump-raw (motion, depth, motion,depth or prevdepth)"); }
            else if (key=="--cadence") { o.cadence=ParseInt(value()); cadenceSet=true; }
            else if (key=="--cadence-phase") { o.cadencePhase=ParseInt(value()); cadenceSet=true; }
            else { const std::string v=value(); if (v=="cadence") o.motionSpanCadence=true; else o.motionSpan=ParseInt(v); }
            continue;
        }
        if (key=="--start-frame" || key=="--exposure-record" || key=="--exposure-replay") { // outside the else-if chain (C1061)
            if (key=="--start-frame") o.startFrame=ParseInt(value()); else (key=="--exposure-record" ? o.exposureRecord : o.exposureReplay)=value();
            continue;
        }
        if (key=="--startup-profile") { o.startupProfile=true; continue; } // outside the else-if chain (C1061)
        if (key=="--texture-cache") { o.textureCache=value(); continue; }
        if (key=="--variants") { o.variants=ParseInt(value()); continue; }
        if (key=="--variant-set") {
            std::string s=value(); std::vector<int> n; size_t start=0;
            do { size_t end=s.find(',',start); n.push_back(ParseInt(s.substr(start,end-start))); if(end==std::string::npos) break; start=end+1; } while(true);
            if (n.size()%3!=0) throw std::runtime_error("--variant-set expects K,ID,VALUE[,K2,ID2,VALUE2...]");
            o.variantSet.clear(); for (size_t k=0;k<n.size();k+=3) o.variantSet.push_back({n[k],n[k+1],n[k+2]});
            continue;
        }
        if (key=="--nr-reset-at") { // outside the else-if chain (C1061)
            std::string s=value(); size_t start=0; o.nrResetAt.clear();
            do { size_t end=s.find(',',start); o.nrResetAt.push_back(ParseInt(s.substr(start,end-start))); if(end==std::string::npos) break; start=end+1; } while(true);
            continue;
        }
        if (key=="--help" || key=="-h") o.help=true;
        else if (key=="--interactive") o.interactive=true;
        else if (key=="--settings") o.settings=value();
        else if (key=="--debug-layer") o.debug=true;
        else if (key=="--gltf") o.gltf=value();
        else if (key=="--hdri") o.hdri=value();
        else if (key=="--camera") o.camera=value();
        else if (key=="--anim-speed") o.animSpeed=ParseFloat(value());
        else if (key=="--anim-start") o.animStart=ParseFloat(value());
        else if (key=="--blas-rebuild") o.blasRebuild=ParseInt(value());
        else if (key=="--follow-node") o.followNode=value();
        else if (key=="--anim-playing") { int n=ParseInt(value()); if(n!=0 && n!=1) throw std::runtime_error("--anim-playing requires 0 or 1"); o.animPlaying=n!=0; }
        else if (key=="--view") o.view=value();
        else if (key=="--motion") o.motion=value();
        else if (key=="--upscaler") o.upscaler=value();
        else if (key=="--render") o.render=value();
        else if (key=="--render-switch-at") o.renderSwitchAt=ParseInt(value());
        else if (key=="--raster-ambient") o.rasterAmbient=ParseFloat(value());
        else if (key=="--snap-deg") o.snapDeg=ParseFloat(value());
        else if (key=="--snap-frames") o.snapFrames=ParseInt(value());
        else if (key=="--snap-hold") o.snapHold=ParseInt(value());
        else if (key=="--hud") o.hud=true;
        else if (key=="--no-instancing") o.instancing=false;
        else if (key=="--cpu-skinning") o.gpuSkinning=false;
        else if (key=="--no-small-culling") o.smallCulling=false;
        else if (key=="--no-occlusion-culling") o.occlusionCulling=false;
        else if (key=="--cull-check") o.cullCheck=true;
        else if (key=="--cull-debug") o.cullDebug=true;
        else if (key=="--cull-small-radius") o.cullSmallRadius=ParseFloat(value());
        else if (key=="--cull-cone-deg") o.cullConeDeg=ParseFloat(value());
        else if (key=="--cull-small-distance") o.cullSmallDistance=ParseFloat(value());
        else if (key=="--wind") o.wind=true;
        else if (key=="--wind-strength") { o.windStrength=ParseFloat(value()); windStrengthSet=true; }
        else if (key=="--raster-sky-ambient") o.rasterSkyAmbient=true;
        else if (key=="--probes") o.probes=true;
        else if (key=="--lamp-tubes") o.lampTubes=true;
        else if (key=="--lamp-shadows") o.lampShadows=true;
        else if (key=="--fxaa") { o.fxaa=true; fxaaSet=true; }
        else if (key=="--probe-spacing") { o.probeSpacing=ParseFloat(value()); probeSet=true; }
        else if (key=="--probe-rays") { o.probeRays=ParseInt(value()); probeSet=true; }
        else if (key=="--probe-dump") { o.probeDump=value(); probeSet=true; }
        else if (key=="--lightmap") o.lightmap=value();
        else if (key=="--csm") o.csm=true;
        else if (key=="--csm-stable") { int n=ParseInt(value()); if(n!=0 && n!=1) throw std::runtime_error("--csm-stable requires 0 or 1"); o.csmStable=n!=0; csmStableSet=true; }
        else if (key=="--csm-distance") { o.csmDistance=ParseFloat(value()); csmDistanceSet=true; }
        else if (key=="--ao") o.ao=value();
        else if (key=="--ao-radius") { o.aoRadius=ParseFloat(value()); aoRadiusSet=true; }
        else if (key=="--ao-strength") { o.aoStrength=ParseFloat(value()); aoStrengthSet=true; }
        else if (key=="--ao-alpha") { o.aoAlpha=ParseFloat(value()); aoAlphaSet=true; }
        else if (key=="--host") o.host=value();
        else if (key=="--core-state-sentinel") {
            o.coreStateSentinel=value();
            if (o.coreStateSentinel!="draw" && o.coreStateSentinel!="compute")
                throw std::runtime_error("Expected draw or compute");
        }
        else if (key=="--core-state-rebind") o.coreStateRebind=true;
        else if (key=="--core-flow") o.coreFlow=true;
        else if (key=="--core-mode") o.coreMode=ParseInt(value());
        else if (key=="--core-temporal") o.coreTemporal=ParseInt(value());
        else if (key=="--core-warp-path") o.coreWarpPath=ParseInt(value());
        else if (key=="--nr") o.nr=value();
        else if (key=="--nr-log") o.nrLog=ParseInt(value());
        else if (key=="--nr-colour") o.nrColour=value();
        else if (key=="--nr-list") o.nrList=value();
        else if (key=="--nr-proxy-format") o.nrProxyFormat=value();
        else if (key=="--switch") o.switchEvery=ParseInt(value());
        else if (key=="--mv-format") o.mvFormat=value();
        else if (key=="--nr-output-pad") {
            std::string s=value(); std::vector<int> n; size_t start=0;
            do { size_t end=s.find(',',start); n.push_back(ParseInt(s.substr(start,end-start))); if(end==std::string::npos) break; start=end+1; } while(true);
            if (n.size()!=2 && n.size()!=4) throw std::runtime_error("--nr-output-pad expects X,Y or X,Y,BX,BY");
            o.nrPadX=n[0]; o.nrPadY=n[1]; o.nrBaseX=n.size()==4 ? n[2] : 0; o.nrBaseY=n.size()==4 ? n[3] : 0;
        }
        else if (key=="--nr-colour-pad") {
            std::string s=value(); size_t comma=s.find(',');
            if (comma==std::string::npos) throw std::runtime_error("--nr-colour-pad expects X,Y[,E]");
            size_t second=s.find(',',comma+1);
            o.nrColourPadX=ParseInt(s.substr(0,comma)); o.nrColourPadY=ParseInt(s.substr(comma+1,second==std::string::npos ? std::string::npos : second-comma-1));
            o.nrColourPadEdge=second==std::string::npos ? 0 : ParseInt(s.substr(second+1));
        }
        else if (key=="--nr-pause") {
            std::string s=value(); std::vector<int> n; size_t start=0;
            do { size_t end=s.find(',',start); n.push_back(ParseInt(s.substr(start,end-start))); if(end==std::string::npos) break; start=end+1; } while(true);
            if (n.empty() || n.size()%2!=0) throw std::runtime_error("--nr-pause expects A,B[,A,B...] ascending");
            o.nrPause.clear();
            for (size_t k=0; k<n.size(); k+=2) {
                const int a=n[k], b=n[k+1];
                if (a<0 || b<=a) throw std::runtime_error("--nr-pause expects A,B[,A,B...] ascending");
                if (!o.nrPause.empty() && a<o.nrPause.back().second) throw std::runtime_error("--nr-pause expects A,B[,A,B...] ascending");
                o.nrPause.emplace_back(a,b);
            }
        }
        else if (key=="--nr-pause-show") o.nrPauseShow=value();
        else if (key=="--nr-recreate") o.nrRecreate=ParseInt(value());
        else if (key=="--nr-release-thread") o.nrReleaseThread=true;
        else if (key=="--set-at") {
            std::string s=value(); std::vector<int> n; size_t start=0;
            do { size_t end=s.find(',',start); n.push_back(ParseInt(s.substr(start,end-start))); if(end==std::string::npos) break; start=end+1; } while(true);
            if (n.empty() || n.size()%3!=0) throw std::runtime_error("--set-at expects F,ID,VALUE[,F2,ID2,VALUE2...]");
            o.setAt.clear();
            for (size_t k=0;k<n.size();k+=3) {
                if (n[k]<0 || n[k+1]<0) throw std::runtime_error("--set-at expects F,ID,VALUE[,F2,ID2,VALUE2...]");
                o.setAt.push_back({n[k],n[k+1],n[k+2]});
            }
        }
        else if (key=="--resize-at") {
            std::string s=value(); std::vector<int> n; size_t start=0;
            do { size_t end=s.find(',',start); n.push_back(ParseInt(s.substr(start,end-start))); if(end==std::string::npos) break; start=end+1; } while(true);
            if (n.size()!=1 && n.size()!=3) throw std::runtime_error("--resize-at expects F or F,W,H");
            o.resizeAt=n[0]; o.resizeWidth=n.size()==3 ? n[1] : 0; o.resizeHeight=n.size()==3 ? n[2] : 0;
        }
        else if (key=="--recreate-swapchain") o.recreateSwapChainAt=ParseInt(value());
        else if (key=="--tonemap") o.tonemap=value();
        else if (key=="--film-slope") o.filmSlope=ParseFloat(value());
        else if (key=="--film-toe") o.filmToe=ParseFloat(value());
        else if (key=="--film-shoulder") o.filmShoulder=ParseFloat(value());
        else if (key=="--film-black-clip") o.filmBlackClip=ParseFloat(value());
        else if (key=="--film-white-clip") o.filmWhiteClip=ParseFloat(value());
        else if (key=="--local-exposure") { int n=ParseInt(value()); if(n!=0 && n!=1) throw std::runtime_error("--local-exposure requires 0 or 1"); o.localExposure=n!=0; }
        else if (key=="--local-exposure-highlights") o.localHighlights=ParseFloat(value());
        else if (key=="--local-exposure-shadows") o.localShadows=ParseFloat(value());
        else if (key=="--local-exposure-detail") o.localDetail=ParseFloat(value());
        else if (key=="--local-exposure-blend") o.localBlend=ParseFloat(value());
        else if (key=="--exposure-metering") o.exposureMetering=value();
        else if (key=="--exposure-min-ev") o.exposureMinEv=ParseFloat(value());
        else if (key=="--exposure-max-ev") o.exposureMaxEv=ParseFloat(value());
        else if (key=="--exposure-curve") o.exposureCurve=ParseVec(value());
        else if (key=="--haze") o.haze=ParseFloat(value());
        else if (key=="--haze-g") o.hazeG=ParseFloat(value());
        else if (key=="--bloom") o.bloom=ParseFloat(value());
        else if (key=="--auto-exposure") { int n=ParseInt(value()); if(n!=0 && n!=1) throw std::runtime_error("--auto-exposure requires 0 or 1"); o.autoExposure=n!=0; }
        else if (key=="--exposure-speed-up") o.exposureSpeedUp=ParseFloat(value());
        else if (key=="--exposure-speed-down") o.exposureSpeedDown=ParseFloat(value());
        else if (key=="--exposure-ev") o.exposureEv=ParseFloat(value());
        else if (key=="--exposure-centre") o.exposureCentre=ParseFloat(value());
        else if (key=="--light-candidates") o.lightCandidates=ParseInt(value());
        else if (key=="--vsync") { int n=ParseInt(value()); if(n!=0 && n!=1) throw std::runtime_error("--vsync requires 0 or 1"); o.vsync=n!=0; }
        else if (key=="--width") o.width=ParseInt(value());
        else if (key=="--height") o.height=ParseInt(value());
        else if (key=="--spp") o.spp=ParseInt(value());
        else if (key=="--bounces") o.bounces=ParseInt(value());
        else if (key=="--render-scale") o.renderScale=ParseFloat(value());
        else if (key=="--fov") o.fov=ParseFloat(value());
        else if (key=="--yaw-speed") o.yawSpeed=ParseFloat(value());
        else if (key=="--move-speed") o.moveSpeed=ParseFloat(value());
        else if (key=="--sway") o.sway=ParseFloat(value());
        else if (key=="--cam-dolly") o.camDolly=ParseFloat(value());
        else if (key=="--cam-lift") o.camLift=ParseFloat(value());
        else if (key=="--cam-pos") { o.camPos=ParseVec(value()); pos=true; }
        else if (key=="--cam-target") { o.camTarget=ParseVec(value()); target=true; }
        else if (key=="--sun-dir") o.sunDir=ParseVec(value());
        else if (key=="--sun-strength") o.sunStrength=ParseFloat(value());
        else if (key=="--sun-angle") o.sunAngle=ParseFloat(value());
        else if (key=="--exposure") o.exposure=ParseFloat(value());
        else if (key=="--albedo") o.albedo=ParseFloat(value());
        else if (key=="--firefly") o.firefly=ParseFloat(value());
        else if (key=="--jitter") { int n=ParseInt(value()); if (n!=0 && n!=1) throw std::runtime_error("--jitter requires 0 or 1"); o.jitter=n!=0; }
        else if (key=="--depth") { auto s=value(); if (s!="standard" && s!="reverse" && s!="linear-negative" && s!="linear-positive") throw std::runtime_error("Invalid depth mode"); o.linearDepth=s=="linear-negative" ? -1 : s=="linear-positive" ? 1 : 0; o.reverse=s=="reverse" || o.linearDepth<0; }
        else if (key=="--dump") {
            std::string s=value(); size_t start=0;
            do { size_t end=s.find(',',start); o.dumps.push_back(ParseInt(s.substr(start,end-start))); if(end==std::string::npos) break; start=end+1; } while(true);
        }
        else if (key=="--dump-format") { o.dumpFormat=value(); if (o.dumpFormat!="bmp" && o.dumpFormat!="png") throw std::runtime_error("Invalid --dump-format (bmp|png)"); }
        else if (!key.starts_with("-") && !frames) { o.frames=ParseInt(key); frames=true; }
        else throw std::runtime_error("Unknown option: "+key);
    }
    auto contains=[](const std::string &s, const std::vector<std::string> &v) { return std::find(v.begin(),v.end(),s)!=v.end(); };
    if (!contains(o.camera,{"static","yaw","forward","strafe","combo","script","follow","snap"})) throw std::runtime_error("Invalid camera mode");
    if (o.snapFrames<1 || o.snapFrames>100000 || o.snapHold<0 || o.snapHold>100000 || !std::isfinite(o.snapDeg) || o.snapDeg<-360 || o.snapDeg>360)
        throw std::runtime_error("--snap-frames requires 1..100000, --snap-hold 0..100000 and --snap-deg -360..360");
    if(o.blasRebuild<1) throw std::runtime_error("--blas-rebuild requires a positive frame interval");
    if (!contains(o.view,{"colour","albedo","normal","depth","motion","accum","noisy","roughness","specular","vegmask"})) throw std::runtime_error("Invalid view");
    if (!contains(o.motion,{"pixels","ndc","none"})) throw std::runtime_error("Invalid motion (pixels|ndc|none)");
    if (!contains(o.upscaler,{"none","sr","rr"})) throw std::runtime_error("Invalid upscaler (none|sr|rr)");
    if (!contains(o.render,{"pathtrace","raster"})) throw std::runtime_error("Invalid --render (pathtrace|raster)");
    if (o.render=="raster" && o.upscaler=="rr")
        throw std::runtime_error("--render raster supports --upscaler none|sr (Ray Reconstruction needs path-traced input)");
    if (o.renderSwitchAt<-1 || o.renderSwitchAt>=o.frames || (o.renderSwitchAt>=0 && o.interactive))
        throw std::runtime_error("--render-switch-at F requires F below the frame count and no --interactive");
    if (o.renderSwitchAt>=0 && o.upscaler=="rr")
        throw std::runtime_error("--render-switch-at enters or leaves raster mode, which supports --upscaler none|sr");
    if (!std::isfinite(o.rasterAmbient) || o.rasterAmbient<0) throw std::runtime_error("--raster-ambient requires a number >= 0");
    if (o.rasterSkyAmbient && (o.render!="raster" || o.hdri.empty())) throw std::runtime_error("--raster-sky-ambient requires --render raster and --hdri");
    if (!std::isfinite(o.cullSmallRadius) || o.cullSmallRadius<0 || o.cullSmallRadius>100) throw std::runtime_error("--cull-small-radius requires 0..100 meters");
    if (!std::isfinite(o.cullConeDeg) || o.cullConeDeg<1 || o.cullConeDeg>360) throw std::runtime_error("--cull-cone-deg requires 1..360 degrees");
    if (!std::isfinite(o.cullSmallDistance) || o.cullSmallDistance<0) throw std::runtime_error("--cull-small-distance requires 0 (off) or more meters");
    if (windStrengthSet && !o.wind) throw std::runtime_error("--wind-strength requires --wind");
    if (!std::isfinite(o.windStrength) || o.windStrength<0 || o.windStrength>100) throw std::runtime_error("--wind-strength requires 0..100");
    if (o.probes && o.render!="raster") throw std::runtime_error("--probes requires --render raster");
    if (probeSet && !o.probes) throw std::runtime_error("--probe-spacing, --probe-rays and --probe-dump require --probes");
    if (o.lampTubes && o.render!="raster") throw std::runtime_error("--lamp-tubes requires --render raster");
    if (o.lampShadows && o.render!="raster") throw std::runtime_error("--lamp-shadows requires --render raster");
    if (fxaaSet && o.render!="raster") throw std::runtime_error("--fxaa requires --render raster");
    if (!std::isfinite(o.probeSpacing) || o.probeSpacing<0.1f || o.probeSpacing>100 || o.probeRays<64 || o.probeRays>16384)
        throw std::runtime_error("--probe-spacing requires 0.1..100 meters and --probe-rays 64..16384");
    if (!o.lightmap.empty() && o.render!="raster") throw std::runtime_error("--lightmap requires --render raster");
    if (o.csm && o.render!="raster") throw std::runtime_error("--csm requires --render raster");
    if (csmStableSet && !o.csm) throw std::runtime_error("--csm-stable requires --csm");
    if (csmDistanceSet && !o.csm) throw std::runtime_error("--csm-distance requires --csm");
    if (!std::isfinite(o.csmDistance) || o.csmDistance<1 || o.csmDistance>10000) throw std::runtime_error("--csm-distance requires 1..10000 meters");
    if (!o.ao.empty() && !contains(o.ao,{"clean","game"})) throw std::runtime_error("Invalid mode, use --ao clean|game");
    if (!o.ao.empty() && o.render!="raster" && o.renderSwitchAt<0) // --render-switch-at may enter raster mode
        throw std::runtime_error("--ao requires --render raster (or --render-switch-at into it)");
    if (aoRadiusSet && o.ao.empty()) throw std::runtime_error("--ao-radius requires --ao");
    if (aoStrengthSet && o.ao.empty()) throw std::runtime_error("--ao-strength requires --ao");
    if (aoAlphaSet && o.ao!="game") throw std::runtime_error("--ao-alpha requires --ao game");
    if (!std::isfinite(o.aoAlpha) || o.aoAlpha<=0 || o.aoAlpha>1) throw std::runtime_error("--ao-alpha requires 0 < A <= 1");
    if (!std::isfinite(o.aoRadius) || o.aoRadius<=0 || o.aoRadius>10 || !std::isfinite(o.aoStrength) || o.aoStrength<0 || o.aoStrength>8)
        throw std::runtime_error("--ao-radius needs 0..10 meters (above 0) and --ao-strength 0..8");
    if (!contains(o.host,{"ngx","core"})) throw std::runtime_error("Invalid --host (ngx|core)");
    if (o.coreMode<0 || o.coreMode>2) throw std::runtime_error("--core-mode requires 0, 1 or 2");
    if (o.coreTemporal<0 || o.coreTemporal>1) throw std::runtime_error("--core-temporal requires 0 or 1");
    if (o.coreWarpPath<0 || o.coreWarpPath>2) throw std::runtime_error("--core-warp-path requires 0, 1 or 2");
    if (o.host=="core" && (o.nr!="native" || o.nrColour!="srgb" || o.upscaler!="sr"))
        throw std::runtime_error("--host core requires --nr native --nr-colour srgb --upscaler sr");
    if (o.coreFlow && o.host!="core") throw std::runtime_error("--core-flow requires --host core (the add-on host takes motion from the frame)");
    if (!contains(o.nr,{"off","native","upscale"})) throw std::runtime_error("Invalid --nr (off|native|upscale)");
    if (o.nr=="upscale" && o.upscaler!="none") throw std::runtime_error("--nr upscale replaces DLSS SR/RR; use it with --upscaler none");
    if (o.nrLog<0 || o.nrLog>2) throw std::runtime_error("--nr-log requires 0, 1 or 2");
    if (!contains(o.nrColour,{"srgb","linear"})) throw std::runtime_error("Invalid --nr-colour (srgb|linear)");
    if (o.switchEvery<0) throw std::runtime_error("--switch requires N >= 0");
    if (!contains(o.nrProxyFormat,{"rgba16f","r10g10b10a2"})) throw std::runtime_error("Invalid --nr-proxy-format (rgba16f|r10g10b10a2)");
    if (o.nrProxyFormat!="rgba16f" && o.nrColour!="srgb") throw std::runtime_error("--nr-proxy-format needs --nr-colour srgb");
    if (!contains(o.nrList,{"direct","compute"})) throw std::runtime_error("Invalid --nr-list (direct|compute)");
    if (o.nrList=="compute" && o.nr!="native") throw std::runtime_error("--nr-list compute requires --nr native");
    if (o.nrColourPadX<0 || o.nrColourPadY<0 || o.nrColourPadX>4096 || o.nrColourPadY>4096) throw std::runtime_error("--nr-colour-pad requires 0..4096");
    if ((o.nrColourPadX || o.nrColourPadY) && o.nrColour!="srgb") throw std::runtime_error("--nr-colour-pad needs --nr-colour srgb (the padded texture is the proxy)");
    if (!o.nrPause.empty() && (o.nr!="native" || o.host!="ngx")) throw std::runtime_error("--nr-pause requires --nr native --host ngx");
    if (!contains(o.nrPauseShow,{"frozen","input"})) throw std::runtime_error("Invalid --nr-pause-show (frozen|input)");
    if (o.nrRecreate>=0 && (o.nr!="native" || o.host!="ngx" || o.nrRecreate>=o.frames)) throw std::runtime_error("--nr-recreate F requires --nr native --host ngx and F below the frame count");
    for (int f:o.nrResetAt) if (f<0 || f>=o.frames || o.nr=="off") throw std::runtime_error("--nr-reset-at F requires --nr and F below the frame count");
    if (o.nrReleaseThread && o.nrRecreate<0) throw std::runtime_error("--nr-release-thread requires --nr-recreate F");
    if (o.resizeAt>=o.frames || o.resizeWidth<0 || o.resizeHeight<0 || o.resizeWidth>16384 || o.resizeHeight>16384 || (o.resizeWidth==0)!=(o.resizeHeight==0) || (o.resizeAt>=0 && o.interactive))
        throw std::runtime_error("--resize-at F[,W,H] requires F below the frame count, 1..16384 for W and H, and no --interactive");
    if (o.recreateSwapChainAt>=o.frames || (o.recreateSwapChainAt>=0 && o.interactive))
        throw std::runtime_error("--recreate-swapchain F requires F below the frame count and no --interactive");
    if (!contains(o.mvFormat,{"rg16f","rgba16f"})) throw std::runtime_error("Invalid --mv-format (rg16f|rgba16f)");
    if (o.nrPadX<0 || o.nrPadY<0 || o.nrPadX>4096 || o.nrPadY>4096 || o.nrBaseX<0 || o.nrBaseY<0 || o.nrBaseX>o.nrPadX || o.nrBaseY>o.nrPadY)
        throw std::runtime_error("--nr-output-pad requires 0 <= BX <= X <= 4096 and 0 <= BY <= Y <= 4096");
    if (!contains(o.tonemap,{"aces","neutral","none","ue-filmic"})) throw std::runtime_error("Invalid tonemap (aces|neutral|none|ue-filmic)");
    if (!(o.filmSlope>0.01f && o.filmSlope<=2) || !(o.filmToe>=0 && o.filmToe<=1) || !(o.filmShoulder>=0 && o.filmShoulder<=1) ||
        !(o.filmBlackClip>=0 && o.filmBlackClip<=1) || !(o.filmWhiteClip>=0 && o.filmWhiteClip<=1))
        throw std::runtime_error("--film-slope requires 0.01..2, --film-toe, --film-shoulder, --film-black-clip, --film-white-clip 0..1");
    if (!(o.localHighlights>=0 && o.localHighlights<=2) || !(o.localShadows>=0 && o.localShadows<=2) || !(o.localDetail>=0 && o.localDetail<=4) ||
        !(o.localBlend>=0 && o.localBlend<=1))
        throw std::runtime_error("--local-exposure-highlights and -shadows require 0..2, -detail 0..4, -blend 0..1");
    if (!contains(o.exposureMetering,{"bench","ue"})) throw std::runtime_error("Invalid --exposure-metering (bench|ue)");
    if (!(o.exposureMinEv>=-16 && o.exposureMaxEv<=32 && o.exposureMinEv<=o.exposureMaxEv))
        throw std::runtime_error("--exposure-min-ev and --exposure-max-ev require -16 <= min <= max <= 32");
    if (!(o.exposureCurve.x>=-16 && o.exposureCurve.y<=32 && o.exposureCurve.x<o.exposureCurve.y && o.exposureCurve.z>=-4 && o.exposureCurve.z<=4))
        throw std::runtime_error("--exposure-curve LOW,HIGH,EV requires -16 <= LOW < HIGH <= 32 and -4 <= EV <= 4");
    if (o.haze<0 || o.hazeG<=-1 || o.hazeG>=1 || o.bloom<0) throw std::runtime_error("Requires haze/bloom >= 0 and -1 < haze-g < 1");
    if (o.exposureSpeedUp<0.01f || o.exposureSpeedUp>100 || o.exposureSpeedDown<0.01f || o.exposureSpeedDown>100)
        throw std::runtime_error("--exposure-speed-up and --exposure-speed-down require 0.01..100");
    if (o.exposureEv<-10 || o.exposureEv>10 || o.exposureCentre<0 || o.exposureCentre>1)
        throw std::runtime_error("--exposure-ev requires -10..10, --exposure-centre 0..1");
    if (o.lightCandidates<1 || o.lightCandidates>1024) throw std::runtime_error("--light-candidates requires 1..1024");
    if (o.frames<1 || o.width<1 || o.height<1 || o.width>16384 || o.height>16384 || o.spp<1 || o.spp>4096 || o.bounces<0 || o.bounces>8)
        throw std::runtime_error("Invalid frame count, dimensions, spp (1..4096), or bounces (0..8)");
    if (o.renderScale<=0 || o.renderScale>1 || o.fov<=1 || o.fov>=179 || o.firefly<=0 || o.sunAngle<0 || o.sunAngle>180 ||
        o.sunStrength<0 || o.albedo<0 || o.exposure<0 || Dot(o.sunDir,o.sunDir)<1e-12f)
        throw std::runtime_error("Invalid rendering or lighting parameter");
    if (pos!=target) throw std::runtime_error("Supply --cam-pos and --cam-target together");
    if (pos && Dot(Cross({0,1,0},o.camTarget-o.camPos),Cross({0,1,0},o.camTarget-o.camPos))<1e-10f)
        throw std::runtime_error("Camera direction must be nonzero and not parallel to world up");
    if (o.motionSpan<1 || o.motionSpan>64) throw MotionSpanError("--motion-span requires 1..64");
    if (o.motionSpan>1 && (o.camera=="follow" || o.interactive))
        throw MotionSpanError("--motion-span K>1 requires a camera that is a function of the frame: no --camera follow, no --interactive");
    if (o.motionSpanCadence) {
        if (o.cadence<2 || o.cadence>8) throw MotionSpanError("--motion-span cadence requires --cadence 2..8");
        if (o.cadencePhase<0 || o.cadencePhase>=o.cadence) throw MotionSpanError("--cadence-phase requires 0..N-1");
        if (o.camera=="follow" || o.interactive)
            throw MotionSpanError("--motion-span cadence requires a camera that is a function of the frame: no --camera follow, no --interactive");
    } else {
        if (cadenceSet) throw MotionSpanError("--cadence and --cadence-phase require --motion-span cadence");
        o.cadence=0;
    }
    for (int n:o.dumps) if(n<0 || n>=o.frames) throw std::runtime_error("Dump frames are zero-based and must be below frame count");
    const int s=o.startFrame; auto before=[s](int f) { return f>=0 && f<s; };
    if (s<0 || s>=o.frames || (s>0 && (o.camera=="follow" || o.interactive)))
        throw std::runtime_error("--start-frame S requires 0 <= S < frames and a camera that is a function of the frame: no --camera follow, no --interactive");
    if (s>0 && o.autoExposure && o.exposureReplay.empty()) // the exposure would restart from its target, unlike the run from 0
        throw std::runtime_error("--start-frame with --auto-exposure 1 requires --exposure-replay FILE (recorded by --exposure-record)");
    if ((!o.exposureRecord.empty() || !o.exposureReplay.empty()) && (o.interactive || !o.exposureRecord.empty()==!o.exposureReplay.empty()))
        throw std::runtime_error("--exposure-record and --exposure-replay are scripted runs only, one of them at a time");
    bool early=before(o.resizeAt) || before(o.renderSwitchAt) || before(o.recreateSwapChainAt) || before(o.nrRecreate);
    for (int n:o.dumps) early|=before(n); for (const auto &p:o.nrPause) early|=p.first<s; for (const auto &a:o.setAt) early|=before(a.frame);
    for (int f:o.nrResetAt) early|=before(f);
    if (o.variants<1 || o.variants>64) throw std::runtime_error("--variants requires 1..64");
    for (const auto &v:o.variantSet) if (v.frame<1 || v.frame>=o.variants || v.id<0) throw std::runtime_error("--variant-set K needs 1 <= K < --variants");
    // A variant replays the frames as a run from S: nothing that happens once, at a frame or per process, may run in it.
    if (o.variants>1 && (o.camera=="follow" || o.interactive || o.host=="core" || o.render!="raster" || !o.dumpRaw.empty() || !o.exposureRecord.empty() ||
        o.resizeAt>=0 || o.renderSwitchAt>=0 || o.recreateSwapChainAt>=0 || o.nrRecreate>=0 || !o.nrPause.empty() || !o.setAt.empty() || !o.nrResetAt.empty() ||
        o.switchEvery>0 || (o.autoExposure && o.exposureReplay.empty())))
        throw std::runtime_error("--variants: raster, --host ngx, a camera of the frame, no --dump-raw, no frame events, auto exposure only replayed");
    if (early) throw std::runtime_error("--start-frame S: --dump, --resize-at, --render-switch-at, --recreate-swapchain, --nr-recreate, --nr-reset-at, --nr-pause and --set-at need frames >= S");
    o.cameraOverride=pos; return o;
}
// Ray Reconstruction needs path-traced input. A raster session that somehow holds RR (the menu's Upscaler combo
// does not offer it there) falls back to SR, so the interactive settings save, which validates, never throws.
inline bool CoerceRasterUpscaler(Options &o)
{
    if (o.render!="raster" || o.upscaler!="rr") return false;
    o.upscaler="sr"; std::fputs("[warn] --render raster does not support DLSS RR; using DLSS SR\n",stderr);
    return true;
}
