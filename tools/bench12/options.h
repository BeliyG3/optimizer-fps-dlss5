#pragma once
#include "math.h"
#include <string>
#include <vector>
#include <stdexcept>
#include <cstdio>
#include <climits>

struct Options {
    int frames=600, width=1920, height=1080, spp=1, bounces=4, lightCandidates=8;
    float renderScale=0.5f, fov=60, yawSpeed=45, moveSpeed=2, sway=0, camDolly=0, camLift=0;
    float sunStrength=1, sunAngle=0.5f, exposure=1.45f, albedo=1, firefly=50;
    float haze=0.012f, hazeG=0.6f, bloom=0.06f;
    bool autoExposure=false;
    int linearDepth=0; // 0 hardware, -1 negative view Z, +1 positive view Z
    float animSpeed=1, animStart=0;
    bool animPlaying=true;
    int blasRebuild=60;
    std::string followNode;
    bool interactive=false, accumulationEnabled=false, accumulationInfinite=false, feedAccumulation=false;
    int accumulationFrames=256;
    std::string settings;
    std::string tonemap="aces";
    Vec3 sunDir{0,-1,0}, camPos{}, camTarget{};
    bool jitter=true, reverse=false, debug=false, cameraOverride=false, help=false, vsync=false;
    std::string upscaler="none", motion="pixels";
    std::string gltf="../bench/assets/lab_scene.glb", hdri, camera="script", view="colour";
    std::vector<int> dumps;
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
    bool pos=o.cameraOverride, target=o.cameraOverride, frames=false;
    for (int i=1; i<argc; ++i) {
        std::string key=argv[i];
        auto value=[&]() -> std::string { if (++i>=argc) throw std::runtime_error("Missing value for "+key); return argv[i]; };
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
        else if (key=="--tonemap") o.tonemap=value();
        else if (key=="--haze") o.haze=ParseFloat(value());
        else if (key=="--haze-g") o.hazeG=ParseFloat(value());
        else if (key=="--bloom") o.bloom=ParseFloat(value());
        else if (key=="--auto-exposure") { int n=ParseInt(value()); if(n!=0 && n!=1) throw std::runtime_error("--auto-exposure requires 0 or 1"); o.autoExposure=n!=0; }
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
        else if (!key.starts_with("-") && !frames) { o.frames=ParseInt(key); frames=true; }
        else throw std::runtime_error("Unknown option: "+key);
    }
    auto contains=[](const std::string &s, const std::vector<std::string> &v) { return std::find(v.begin(),v.end(),s)!=v.end(); };
    if (!contains(o.camera,{"static","yaw","forward","strafe","combo","script","follow"})) throw std::runtime_error("Invalid camera mode");
    if(o.blasRebuild<1) throw std::runtime_error("--blas-rebuild requires a positive frame interval");
    if (!contains(o.view,{"colour","albedo","normal","depth","motion","accum","noisy","roughness","specular"})) throw std::runtime_error("Invalid view");
    if (!contains(o.motion,{"pixels","ndc"})) throw std::runtime_error("Invalid motion (pixels|ndc)");
    if (!contains(o.upscaler,{"none","sr","rr"})) throw std::runtime_error("Invalid upscaler (none|sr|rr)");
    if (!contains(o.tonemap,{"aces","neutral"})) throw std::runtime_error("Invalid tonemap (aces|neutral)");
    if (o.haze<0 || o.hazeG<=-1 || o.hazeG>=1 || o.bloom<0) throw std::runtime_error("Requires haze/bloom >= 0 and -1 < haze-g < 1");
    if (o.lightCandidates<1 || o.lightCandidates>1024) throw std::runtime_error("--light-candidates requires 1..1024");
    if (o.frames<1 || o.width<1 || o.height<1 || o.width>16384 || o.height>16384 || o.spp<1 || o.spp>4096 || o.bounces<0 || o.bounces>8)
        throw std::runtime_error("Invalid frame count, dimensions, spp (1..4096), or bounces (0..8)");
    if (o.renderScale<=0 || o.renderScale>1 || o.fov<=1 || o.fov>=179 || o.firefly<=0 || o.sunAngle<0 || o.sunAngle>180 ||
        o.sunStrength<0 || o.albedo<0 || o.exposure<0 || Dot(o.sunDir,o.sunDir)<1e-12f)
        throw std::runtime_error("Invalid rendering or lighting parameter");
    if (pos!=target) throw std::runtime_error("Supply --cam-pos and --cam-target together");
    if (pos && Dot(Cross({0,1,0},o.camTarget-o.camPos),Cross({0,1,0},o.camTarget-o.camPos))<1e-10f)
        throw std::runtime_error("Camera direction must be nonzero and not parallel to world up");
    for (int n:o.dumps) if(n<0 || n>=o.frames) throw std::runtime_error("Dump frames are zero-based and must be below frame count");
    o.cameraOverride=pos; return o;
}
inline void PrintUsage()
{
    std::puts("pw_bench12 [frames=600] --gltf file.glb [--hdri file.hdr]\n"
        "  --interactive --settings file (interactive defaults to settings.json beside exe)\n"
        "  --width 1920 --height 1080 --render-scale 0.5 --spp 1 --bounces 0..8\n"
        "  --camera static|yaw|forward|strafe|combo|script|follow --yaw-speed 45 --move-speed 2 --sway 0\n"
        "  --anim-speed 1 --anim-start 0 --anim-playing 0|1 --blas-rebuild 60 --follow-node NAME\n"
        "  --cam-dolly 0 --cam-lift 0 --cam-pos x,y,z --cam-target x,y,z --fov 60\n"
        "  --sun-dir x,y,z --sun-strength 1 --sun-angle 0.5 --exposure 1.45 --albedo 1\n"
        "  --firefly 50 --jitter 0|1 --depth standard|reverse|linear-negative|linear-positive --motion pixels|ndc\n"
        "  --haze 0.012 --haze-g 0.6 --tonemap aces|neutral --bloom 0.06 --auto-exposure 0|1\n"
        "  --light-candidates 8 --vsync 0|1 --upscaler none|sr|rr (quality from render scale)\n"
        "  --view colour|noisy|accum|depth|motion|normal|roughness|albedo|specular --dump N[,N...] --debug-layer");
}
