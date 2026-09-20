#include "animation.h"
#include "tests_animation_baseline.h"
#include <chrono>
#include <filesystem>
#include <numeric>
#include <stdexcept>
#include <windows.h>

namespace {
using Clock=std::chrono::steady_clock;
double Error(Vec3 a,Vec3 b) { return std::max({std::abs(double(a.x)-b.x),std::abs(double(a.y)-b.y),std::abs(double(a.z)-b.z)}); }
}

void AnimationWriteCombinedBenchmark(const std::filesystem::path &path)
{
    if(!std::filesystem::exists(path)) return;
    // D3D12 UPLOAD heaps are normally write-combined. Exercise that CPU memory
    // policy without creating a device; never read the mapped output while timing.
    std::unique_ptr<TriVertex,void(*)(TriVertex *)> output(nullptr,[](TriVertex *p) { if(p) VirtualFree(p,0,MEM_RELEASE); });
    Animation animation;
    if(!pwgltf::Load(path.string().c_str(),animation.source)) throw std::runtime_error(animation.source.error);
    std::vector<bool> opaque;
    for(auto ref:pwgltf::Primitives(animation.source,1)) {
        const auto &p=animation.source.meshes[animation.source.nodes[ref.node].mesh].primitives[ref.primitive];
        size_t valid=0; for(auto i:p.indices) if(i<p.positions.size()) ++valid;
        opaque.insert(opaque.end(),valid/3,!p.alphaMask);
    }
    if(opaque.empty()) return;
    std::vector<unsigned> order(opaque.size()); std::iota(order.begin(),order.end(),0u);
    std::stable_partition(order.begin(),order.end(),[&](unsigned i) { return opaque[i]; });
    output.reset(static_cast<TriVertex *>(VirtualAlloc(nullptr,order.size()*3*sizeof(TriVertex),
        MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE|PAGE_WRITECOMBINE)));
    if(!output) throw std::runtime_error("Write-combined benchmark allocation failed: "+std::to_string(GetLastError()));
    animation.Start(output.get(),order); animation.Evaluate(0,0);
    const auto start=Clock::now();
    for(int frame=1;frame<=60;++frame) animation.Evaluate(float(frame)/60,float(frame-1)/60);
    const double playing=std::chrono::duration<double,std::milli>(Clock::now()-start).count()/60;
    const auto pauseStart=Clock::now(); animation.Evaluate(1,1);
    const double pauseTransition=std::chrono::duration<double,std::milli>(Clock::now()-pauseStart).count();
    const auto pausedStart=Clock::now();
    for(int frame=0;frame<60;++frame) animation.Evaluate(1,1);
    const double paused=std::chrono::duration<double,std::milli>(Clock::now()-pausedStart).count()/60;
    std::printf("[bench] write-combined direct output, 60 frames: playing %.4f ms/frame, paused %.6f ms/frame (pause transition %.4f ms)\n",
        playing,paused,pauseTransition);
}
void AnimationBenchmark(const std::filesystem::path &path)
{
    if(!std::filesystem::exists(path)) { std::puts("[bench] lab scene absent; animation benchmark skipped"); return; }
    Animation animation;
    if(!pwgltf::Load(path.string().c_str(),animation.source)) throw std::runtime_error(animation.source.error);
    const auto refs=pwgltf::Primitives(animation.source,1);
    std::vector<bool> opaque;
    size_t unique=0;
    for(auto ref:refs) {
        const auto &p=animation.source.meshes[animation.source.nodes[ref.node].mesh].primitives[ref.primitive];
        size_t valid=0; for(auto i:p.indices) if(i<p.positions.size()) ++valid;
        if(valid%3) throw std::runtime_error("Benchmark requires triangle primitives");
        opaque.insert(opaque.end(),valid/3,!p.alphaMask); unique+=p.positions.size();
    }
    std::vector<unsigned> order(opaque.size()); std::iota(order.begin(),order.end(),0u);
    std::stable_partition(order.begin(),order.end(),[&](unsigned i) { return opaque[i]; });
    std::vector<TriVertex> output(order.size()*3),reference(output.size());
    std::printf("[bench] lab: %zu dynamic triangles, %zu unique vertices, %zu primitives, %u logical CPUs\n",
        order.size(),unique,refs.size(),std::thread::hardware_concurrency());
    double maxPosition=0,maxNormal=0;
    auto compare=[&] {
        for(size_t i=0;i<output.size();++i) {
            maxPosition=std::max({maxPosition,Error(output[i].pos,reference[i].pos),Error(output[i].prevPos,reference[i].prevPos)});
            maxNormal=std::max(maxNormal,Error(output[i].normal,reference[i].normal));
            if(output[i].uv[0]!=reference[i].uv[0] || output[i].uv[1]!=reference[i].uv[1]) throw std::runtime_error("Animation UV/order mismatch");
        }
        if(maxPosition>0.0001 || maxNormal>0.0001) throw std::runtime_error("Animation differs from legacy deformation");
    };
    AnimationBaseline baseline(animation.source);
    baseline.Evaluate(0,0,reference.data(),order);
    auto legacy=[&](bool paused) {
        const auto start=Clock::now();
        for(int frame=1;frame<=60;++frame)
            baseline.Evaluate(paused ? 1.0f : float(frame)/60,paused ? 1.0f : float(frame-1)/60,reference.data(),order);
        return std::chrono::duration<double,std::milli>(Clock::now()-start).count()/60;
    };
    const double baselinePlaying=legacy(false),baselinePaused=legacy(true);
    animation.Start(output.data(),order); animation.Evaluate(0,0);
    auto optimized=[&](bool paused) {
        const auto start=Clock::now();
        for(int frame=1;frame<=60;++frame)
            animation.Evaluate(paused ? 1.0f : float(frame)/60,paused ? 1.0f : float(frame-1)/60);
        return std::chrono::duration<double,std::milli>(Clock::now()-start).count()/60;
    };
    const double playing=optimized(false);
    baseline.Evaluate(1,59.0f/60,reference.data(),order); compare();
    if(!output.empty() && (!animation.Evaluate(1,1) || animation.geometryChanged)) throw std::runtime_error("Pause transition must upload without refit");
    const double paused=optimized(true);
    baseline.Evaluate(1,1,reference.data(),order); compare();
    // Untimed serial-oracle checks cover consecutive playback, reverse, wrapped previous
    // times, discontinuous seeks and same-time changes of the previous pose.
    const float end=animation.source.animationEnd;
    const float samples[][2]={{0,0},{0.25f,0},{0.5f,0.25f},{0.1f,end},{0.1f,0.5f},{0.7f,0.7f},{0.6f,0.7f}};
    for(const auto &sample:samples) {
        animation.Evaluate(sample[0],sample[1]);
        baseline.Evaluate(sample[0],sample[1],reference.data(),order); compare();
    }
    std::printf("[bench] legacy evaluation + upload packing, 60 frames: playing %.4f ms/frame, paused %.4f ms/frame\n",baselinePlaying,baselinePaused);
    std::printf("[bench] direct animation output, 60 frames: playing %.4f ms/frame, paused %.6f ms/frame\n",playing,paused);
    std::printf("[bench] legacy comparison: max position error %.9g, normal error %.9g (UV exact)\n",maxPosition,maxNormal);
}
