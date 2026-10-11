#include "application.h"
#include "menu.h"
#include "flight.h"
#include "animation_time.h"
#include "follow_camera.h"
#include "usage.h"
#include "startup_profile.h"
#include "ngx_nr_setting_at.h"
#include <chrono>
#include <memory>

namespace {
bool ExplicitCamera(int argc,char **argv)
{
    for(int i=1;i<argc;++i) {
        std::string key=argv[i];
        if(key=="--camera" || key=="--cam-pos" || key=="--cam-target" || key=="--cam-dolly" || key=="--cam-lift" || key=="--sway") return true;
    }
    return false;
}
// Called before every settings save. SaveSettings validates its own output, so the combination it rejects is fixed first.
void Capture(Settings &settings,const Scene &scene)
{
    settings.lamps=scene.groups; settings.options.animStart=scene.animationTime; CoerceRasterUpscaler(settings.options);
}
// --variants K: before variant k (1..K-1) the window [S, frames) starts again as a run from S would start it: the clock
// advanced S-1 times from the start, feature 18 released and created after frame S's present, the frame's history reset,
// k's --variant-set settings applied, and the dumps named v<k>_dump_<frame>.
void StartVariant(int variant, Device &device, Pathtrace &trace, AnimationClock &clock, const Options &o)
{
    device.Wait();
    clock.time=double(o.animStart); // as the AnimationClock{animStart, duration} built before the loop
    for(int f=1;f<o.startFrame;++f) clock.Advance(AnimationDelta(false,0),o.animSpeed,o.animPlaying,false);
    trace.Rewind(device);
    for(const auto &set:o.variantSet) if(set.frame==variant)
        if(SetAddonSettingAt(o.startFrame,std::uint32_t(set.id),set.value)!=0) throw std::runtime_error("--variant-set: the add-on refused a setting");
    device.dumpPrefix="v"+std::to_string(variant)+"_";
    std::printf("[variant] %d: frames %d..%d again\n",variant,o.startFrame,o.frames-1);
}
}
int RunApplication(int argc, char **argv)
{
    auto command=ParseOptions(argc,argv);
    if(command.help) { PrintUsage(); return 0; }
    Settings settings; settings.options.interactive=command.interactive;
    settings.options.vsync=command.interactive;
    auto path=command.settings.empty() ? ExecutableDirectory()/"settings.json" : std::filesystem::path(command.settings);
    if(!command.settings.empty() || (command.interactive && std::filesystem::exists(path))) settings=LoadSettings(path,settings);
    settings.options=CommandLineOptions(argc,argv,settings.options);
    auto &o=settings.options;
    if(ExplicitCamera(argc,argv)) settings.hasPose=false;
    if(!o.interactive && settings.hasPose) {
        o.cameraOverride=true; o.camPos=settings.pose.eye; o.camTarget=settings.pose.target; o.camDolly=o.camLift=o.sway=0;
    }
    Device device(unsigned(o.width),unsigned(o.height),o.debug,o.vsync,o.interactive);
    Scene scene(device,o); scene.ApplyLamps(settings.lamps); scene.UpdateLamps(device);
    AnimationClock animationClock{double(o.animStart),scene.animationDuration};
    FollowCamera follow;
    Vec3 followPosition{},followHeading{};
    if(o.camera=="follow" && !scene.FollowPose(scene.animationTime,followPosition,followHeading))
        throw std::runtime_error("Follow camera requires --follow-node NAME or a node containing hero/girl");
    Accel accel(device,scene); StartupMark("acceleration structures (BLAS, TLAS)");
    Pathtrace trace(device); StartupMark("pipelines (path tracer, raster, display)");
    trace.UseSkyReflections(device,scene); // every run: raster frames read them, --sky-reflections or the menu adds them
    StartupMark("sky reflections");
    if(o.rasterSkyAmbient) trace.UseSkyAmbient(device,o);
    StartupMark("sky ambient (SH9 of the HDRI)");
    if(o.probes) trace.UseProbes(device,BuildProbes(device,scene,accel,o));
    StartupMark("probes");
    if(o.lampTubes) trace.UseLampTubes(device,scene);
    if(o.lampShadows) trace.UseLampShadows(device,scene,o);
    if(o.fxaa) trace.UseFxaa(device);
    if(!o.lightmap.empty()) trace.UseLightmap(device,scene,o);
    if(o.csm) trace.UseCsm(device,o);
    if(!o.ao.empty()) trace.UseAo(device,o);
    trace.UseExposureTrack(o);
    StartupMark("lamp tubes and shadows, FXAA, lightmap, CSM, AO, exposure track");
    Options homeOptions=o; homeOptions.camera="static"; homeOptions.cameraOverride=false; homeOptions.sway=0;
    CameraState home=CameraAt(0,homeOptions,scene.cameraAnchor);
    if(!settings.hasPose) settings.pose=CameraAt(0,o,scene.cameraAnchor);
    std::unique_ptr<Menu> menu;
    if(o.interactive) menu=std::make_unique<Menu>(device);
    Flight flight; bool lampsDirty=false;
    auto lastLampUpdate=std::chrono::steady_clock::now()-std::chrono::milliseconds(100);
    // --start-frame S: the clock advanced as frames 1..S-1 did, in the same steps (frame S advances it once more below), so
    // the animation time is the run from 0's bit for bit. Camera, wind, jitter, --motion-span and the dumps are functions of
    // the frame; the scene still holds frame 0's pose, which the probes were traced in. No NR / RR / core history (reset).
    for(int f=1;f<o.startFrame;++f) animationClock.Advance(AnimationDelta(false,0),o.animSpeed,o.animPlaying,false);
    int frame=o.startFrame, variant=0;
    if(o.variants>1) device.dumpPrefix="v0_";
    double animationMilliseconds=0,blasMilliseconds=0,loopMilliseconds=0,waitMilliseconds=0,renderMilliseconds=0,poseMilliseconds=0; int loopFrames=0;
    auto lastFrame=std::chrono::steady_clock::now();
    for(; (o.interactive || frame<o.frames) && device.Pump();++frame) {
        const auto now=std::chrono::steady_clock::now();
        device.wallFrameMs=std::chrono::duration<double,std::milli>(now-lastFrame).count();
        const double elapsed=std::chrono::duration<double>(now-lastFrame).count(), delta=AnimationDelta(o.interactive,elapsed);
        lastFrame=now;
        bool dump=std::find(o.dumps.begin(),o.dumps.end(),frame)!=o.dumps.end();
        if(menu) {
            RECT client{}; GetClientRect(device.Window(),&client);
            if(IsIconic(device.Window()) || client.right<=0 || client.bottom<=0) {
                flight.ReleaseMouse(); Sleep(20); --frame; continue;
            }
            device.Resize(unsigned(client.right),unsigned(client.bottom));
            menu->Begin();
            // Movement retains previous-frame matrices; only accumulation is restarted.
            if(flight.Update(device,settings,home) && o.camera=="follow") o.camera="static";
            auto action=menu->Draw(device,scene,trace,settings);
            if(action.changed) trace.Reset();
            if(action.lamps) lampsDirty=true;
            if(action.save) {
                try { Capture(settings,scene); SaveSettings(path,settings); menu->status="Settings saved."; }
                catch(const std::exception &error) { menu->status=error.what(); }
            }
            if(action.reload) {
                try {
                    auto loaded=LoadSettings(path,settings);
                    // Explicit CLI switches continue to take precedence on reload.
                    loaded.options=CommandLineOptions(argc,argv,loaded.options);
                    if(ExplicitCamera(argc,argv)) { loaded.pose=CameraAt(0,loaded.options,scene.cameraAnchor); loaded.hasPose=true; }
                    settings=std::move(loaded); scene.ApplyLamps(settings.lamps); lampsDirty=true; trace.Reset();
                    scene.animationTime=o.animStart; scene.animationSeek=true;
                    menu->status="Settings reloaded.";
                } catch(const std::exception &error) { menu->status=error.what(); }
            }
            if(lampsDirty && std::chrono::steady_clock::now()-lastLampUpdate>=std::chrono::milliseconds(100)) {
                scene.UpdateLamps(device); trace.Reset(); lampsDirty=false; lastLampUpdate=std::chrono::steady_clock::now();
            }
            dump|=action.dump; device.SetVsync(o.vsync);
        }
        if(frame==o.resizeAt) { // a game's resize (a pause menu's resolution change, a fullscreen toggle)
            const unsigned w=o.resizeWidth ? unsigned(o.resizeWidth) : device.width, h=o.resizeHeight ? unsigned(o.resizeHeight) : device.height;
            std::printf("[info] frame %d: swap chain resized %ux%u -> %ux%u\n",frame,device.width,device.height,w,h);
            device.Resize(w,h,true);
        }
        if(frame==o.renderSwitchAt) { // a settings change between frames: the other render mode from this frame on
            o.render=o.render=="raster" ? "pathtrace" : "raster";
            std::printf("[info] frame %d: render mode switched to %s\n",frame,o.render.c_str());
        }
        if(frame==o.recreateSwapChainAt) { // a game's display-mode switch: ReShade's destroy_swapchain (not a resize)
            std::printf("[info] frame %d: swap chain recreated %ux%u\n",frame,device.width,device.height);
            device.RecreateSwapChain();
        }
        float previousTime=animationClock.Current();
        if(scene.animationSeek) {
            animationClock.Seek(scene.animationTime); previousTime=animationClock.Current(); trace.Reset(); follow.Reset();
        } else if(frame>0 || scene.animationStep) animationClock.Advance(delta,o.animSpeed,o.animPlaying,scene.animationStep);
        // --motion-span K: the previous time, wind frame and camera are those of frame t-K (clamped to 0), so the motion is
        // the exact offset to that frame; animation time is then a function of the frame (fixed 1/60 delta, no --interactive).
        const int span=o.SpanAt(frame);
        const int spanFrom=std::max(frame-span,0);
        if(span>1 && o.animPlaying) previousTime=LoopAnimation(double(o.animStart)+double(spanFrom)*o.animSpeed/60,animationClock.duration);
        scene.Animate(animationClock.Current(),previousTime,unsigned(frame),unsigned(spanFrom)); // the wind's clock is the frame index
        scene.animationSeek=scene.animationStep=false;
        const CameraState *pose=menu ? &settings.pose : nullptr;
        if(o.camera=="follow" && scene.FollowPose(scene.animationTime,followPosition,followHeading)) {
            settings.pose=follow.Update(followPosition,followHeading,o,float(delta)); settings.hasPose=true; pose=&settings.pose;
        } else follow.Reset();
        const auto renderStart=std::chrono::steady_clock::now();
        trace.Render(device,scene,accel,o,frame,dump,pose,menu ? Menu::Render : nullptr,float(delta),ExposureSeconds(o.interactive,elapsed));
        if(frame>=o.startFrame+2 && !dump) {
            renderMilliseconds+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-renderStart).count();
            poseMilliseconds+=scene.animationCpuMs; // the main loop's wall time per frame, past the first frames and without the dumps' readback
            loopMilliseconds+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-now).count();
            waitMilliseconds+=device.lastWaitMs; ++loopFrames;
        }
        if(frame==o.startFrame && !variant) StartupMark("first frame: render and present");
        if(frame==o.startFrame+1 && !variant) StartupMark("second frame (feature 18 created, first NR evaluate)");
        animationMilliseconds+=scene.animationCpuMs;
        blasMilliseconds+=scene.animationRefitNeeded ? device.lastBlasMs : 0;
        if(frame==o.frames-1 && variant+1<o.variants) { StartVariant(++variant,device,trace,animationClock,o); frame=o.startFrame-1; }
    }
    flight.ReleaseMouse();
    const int ran=(frame-o.startFrame)*(variant+1);
    device.FinishDumps();
    StartupMark("the remaining frames");
    if(menu) { Capture(settings,scene); SaveSettings(path,settings); }
    device.Wait(); device.PrintMessages(); device.ReportTimings();
    std::printf("[info] cpu animation (%s skinning): avg %.3f ms, blas refit %.3f ms\n",scene.gpuSkinned ? "gpu" : "cpu",
        ran ? animationMilliseconds/ran : 0,ran ? blasMilliseconds/ran : 0);
    if(loopFrames) std::printf("[info] main loop: avg %.3f ms per frame, of which %.3f ms waiting for the GPU (cpu part %.3f ms), %d frames\n",
        loopMilliseconds/loopFrames,waitMilliseconds/loopFrames,(loopMilliseconds-waitMilliseconds)/loopFrames,loopFrames);
    // The CPU part split: the animation (CPU skinning, or the poses and palettes), recording and submitting the frame (culling
    // loops, uploads, present), and the rest of the loop (messages, menu, camera).
    if(loopFrames) std::printf("[info] cpu stages: animation %.3f ms, record+submit+present %.3f ms, other %.3f ms\n",
        poseMilliseconds/loopFrames,(renderMilliseconds-waitMilliseconds)/loopFrames,(loopMilliseconds-renderMilliseconds-poseMilliseconds)/loopFrames);
    const bool ngxUsed=o.upscaler!="none" || o.nr!="off";
    trace.Shutdown(device);
    StartupMark("shutdown (NR, NGX)");
    if(o.startupProfile) { StartupProfile::Get().Add("dump writes (readback, encode, files)",trace.dumpWriteSeconds); StartupProfile::Get().Print(); }
    std::printf("[info] bench finished after %d frames, device ok\n",ran);
    if(ngxUsed) {
        // Driver 616.92: after Ray Reconstruction has run, the process dies with an access violation
        // during ordinary teardown, after NGX release/destroy/Shutdown1 have all returned success
        // (a driver worker outlives them). Everything is saved and flushed by now, so leave without
        // running the remaining destructors. The directly hosted NR runtime is never shut down either
        // (its feature is released, the DLL stays loaded), so it takes the same exit.
        std::fflush(stdout); std::fflush(stderr);
        TerminateProcess(GetCurrentProcess(),0);
    }
    return 0;
}
