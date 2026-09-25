#include "application.h"
#include "menu.h"
#include "flight.h"
#include "animation_time.h"
#include "follow_camera.h"
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
void Capture(Settings &settings,const Scene &scene) { settings.lamps=scene.groups; settings.options.animStart=scene.animationTime; }
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
    Accel accel(device,scene); Pathtrace trace(device);
    Options homeOptions=o; homeOptions.camera="static"; homeOptions.cameraOverride=false; homeOptions.sway=0;
    CameraState home=CameraAt(0,homeOptions,scene.cameraAnchor);
    if(!settings.hasPose) settings.pose=CameraAt(0,o,scene.cameraAnchor);
    std::unique_ptr<Menu> menu;
    if(o.interactive) menu=std::make_unique<Menu>(device);
    Flight flight; bool lampsDirty=false;
    auto lastLampUpdate=std::chrono::steady_clock::now()-std::chrono::milliseconds(100);
    int frame=0;
    double animationMilliseconds=0,blasMilliseconds=0;
    auto lastFrame=std::chrono::steady_clock::now();
    for(; (o.interactive || frame<o.frames) && device.Pump();++frame) {
        const auto now=std::chrono::steady_clock::now();
        device.wallFrameMs=std::chrono::duration<double,std::milli>(now-lastFrame).count();
        const double delta=AnimationDelta(o.interactive,std::chrono::duration<double>(now-lastFrame).count()); lastFrame=now;
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
        float previousTime=animationClock.Current();
        if(scene.animationSeek) {
            animationClock.Seek(scene.animationTime); previousTime=animationClock.Current(); trace.Reset(); follow.Reset();
        } else if(frame>0 || scene.animationStep) animationClock.Advance(delta,o.animSpeed,o.animPlaying,scene.animationStep);
        scene.Animate(animationClock.Current(),previousTime);
        scene.animationSeek=scene.animationStep=false;
        const CameraState *pose=menu ? &settings.pose : nullptr;
        if(o.camera=="follow" && scene.FollowPose(scene.animationTime,followPosition,followHeading)) {
            settings.pose=follow.Update(followPosition,followHeading,o,float(delta)); settings.hasPose=true; pose=&settings.pose;
        } else follow.Reset();
        trace.Render(device,scene,accel,o,frame,dump,pose,menu ? Menu::Render : nullptr,float(delta));
        animationMilliseconds+=scene.animationCpuMs;
        blasMilliseconds+=scene.animationRefitNeeded ? device.lastBlasMs : 0;
    }
    flight.ReleaseMouse();
    if(menu) { Capture(settings,scene); SaveSettings(path,settings); }
    device.Wait(); device.PrintMessages(); device.ReportTimings();
    std::printf("[info] cpu animation: avg %.3f ms, blas refit %.3f ms\n",
        frame ? animationMilliseconds/frame : 0,frame ? blasMilliseconds/frame : 0);
    const bool ngxUsed=o.upscaler!="none" || o.nr!="off";
    trace.Shutdown(device);
    std::printf("[info] bench finished after %d frames, device ok\n",frame);
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
