// pw_bench: a tiny D3D11 "game" with native DLSS through the driver's NGX core. Put Reshade64.dll as
// dxgi.dll, the DLSS 5 bridge, renodx-dlss5, optimizer-fps-dlss5.addon64 (+ forwarder and shaders), the
// DLSS/DLSSNR snippets and the configs beside it, and the whole BG3 chain runs here without the game.
// Exit code: 0 after N frames with a live device, 2 when the D3D11 device is removed, 1 on setup errors,
// 3 when the bridge/ReShade logs show the bridge stopped or the D3D12 device was removed.
//
// Usage: pw_bench [frames] [options] - every option is listed in bench_options.cpp.
// Sources: bench_options (command line), bench_device (window, device, swap chain, targets),
// bench_scene with bench_pipeline / bench_lighting / bench_boxes (scene state, HDRI, procedural boxes),
// bench_gltf_scene (--gltf), bench_camera, bench_render / bench_frame (drawing a frame), bench_shaders
// (HLSL), bench_hdri (.hdr loading), bench_ngx (DLSS), bench_addon (add-on exports, log verdict),
// bench_capture (--measure, --dump), bench_timing (--fps-cap, --frametime).
#include "bench_addon.h"
#include "bench_capture.h"
#include "bench_device.h"
#include "bench_gltf_scene.h"
#include "bench_ngx.h"
#include "bench_options.h"
#include "bench_render.h"
#include "bench_scene.h"
#include "bench_timing.h"

#include <cstdio>
#include <cwchar>

int main(int argc, char **argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0); // unbuffered: the last line survives a crash
    BenchOptions o;
    if (!ParseOptions(argc, argv, o)) return 1;
    GltfScene gl;
    if (o.gltfPath && !LoadGltf(o, gl)) return 1;
    PrintSceneSummary(o);
    wchar_t exeDir[MAX_PATH]{}; GetModuleFileNameW(nullptr, exeDir, MAX_PATH); if (wchar_t *s = wcsrchr(exeDir, L'\\')) *s = 0;
    SetCurrentDirectoryW(exeDir);

    BenchDevice d;
    if (!CreateBenchDevice(o, d)) return 1;
    Capture capture;
    if (!CreateCaptureTargets(o, d, capture)) return 1;
    BenchScene scene;
    if (!CreatePipeline(o, d, scene)) return 1;
    CreateLighting(o, d, scene);
    if (o.gltfPath && !CreateGltfResources(d, gl)) return 1;
    BuildBoxes(scene);

    // ---- DLSS feature ----
    DlssFeature dlss;
    if (!o.reference && !CreateDlss(exeDir, d, dlss)) return 1;

    // ---- add-on control exports ----
    AddonControl addon;
    if (!FindAddonExports(o, addon)) return 1;

    int frame = 0;
    for (; frame < o.frames; ++frame) {
        MSG msg; while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); if (msg.message == WM_QUIT) frame = o.frames; }
        AddonFrame(o, addon, frame);
        float jx = 0.0f, jy = 0.0f;
        Jitter(frame, &jx, &jy);
        const float clearDepth = (o.flat || g_standardDepth) ? 1.0f : 0.0f;
        d.ctx->ClearDepthStencilView(d.dsv.Get(), D3D11_CLEAR_DEPTH, clearDepth, 0);
        if (o.flat) RenderFlat(o, d, scene, frame);
        else if (o.reference) RenderReference(o, d, scene, gl, frame);
        else RenderScene3D(o, d, scene, gl, frame, jx, jy, 1.0f, false, kRenderW, kRenderH);

        const bool paused = NrPaused(o, frame);
        if (!o.reference && !paused) EvaluateDlss(o, d, dlss, frame, jx, jy);

        PresentBlit(o, d, scene, paused);
        if (o.dumpBack) for (int n : o.dumpFrames) if (frame == n) DumpBackBuffer(o, d, capture, frame);
        d.sc->Present(0, 0);
        if (o.fpsCap > 0.0) PaceFrame(o.fpsCap, o.fpsJitterMs);
        RecordFrameTime(frame, o.frames, o.frametime);

        if (o.measureEvery > 0 && frame > 0 && frame % o.measureEvery == 0) MeasureOutput(d, capture, frame);
        if (!o.dumpBack) for (int n : o.dumpFrames) if (frame == n) DumpOutput(d, capture, frame);
        const HRESULT removed = d.dev->GetDeviceRemovedReason();
        if (FAILED(removed)) { std::printf("[fail] frame %d: D3D11 device removed 0x%08lX\n", frame, (unsigned long) removed); std::fflush(stdout); TerminateProcess(GetCurrentProcess(), 2); }
        if (frame % 120 == 0) std::printf("[info] frame %d ok\n", frame);
    }
    PrintMeasureAverage(capture);
    ReportD3D11Messages(o, d);
    int verdict = o.reference ? 0 : VerdictFromLogs();
    if (gl.skinFrames > 0) std::printf("[info] glTF deform (morphs + CPU skinning + upload prep): %.2f ms/frame over %d frames, %zu vertices\n", gl.skinMsTotal / gl.skinFrames, gl.skinFrames, gl.gVerts.size());
    if (verdict == 0) std::printf("[info] bench finished after %d frames (%d layout switches), device ok\n", frame, addon.switches);
    else std::printf("[fail] bench finished after %d frames (%d layout switches) but the chain broke (see above)\n", frame, addon.switches);
    std::fflush(stdout);
    TerminateProcess(GetCurrentProcess(), verdict);
    return verdict;
}
