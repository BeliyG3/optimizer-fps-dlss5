#include "animation.h"
#include "animation_deform.h"
#include <chrono>
#include <stdexcept>

Animation::~Animation()
{
    { std::lock_guard lock(mutex); stopping=true; }
    wake.notify_all();
    for(auto &worker:workers) if(worker->thread.joinable()) worker->thread.join();
}
void Animation::Start(TriVertex *destination,const std::vector<unsigned> &order)
{
    if(output) throw std::logic_error("Animation already started");
    output=destination;
    auto refs=pwgltf::Primitives(source,1);
    if(refs.empty()) return;
    currentPose.Initialize(source,refs); previousPose.Initialize(source,refs);
    if(!output) throw std::invalid_argument("Animation output is null");
    size_t count=0;
    for(auto ref:refs) {
        const auto &p=source.meshes[source.nodes[ref.node].mesh].primitives[ref.primitive];
        for(auto i:p.indices) if(i<p.positions.size()) ++count;
    }
    if(count%3 || (!order.empty() && order.size()!=count/3)) throw std::runtime_error("Animation triangle order size");
    std::vector<size_t> inverse(count/3);
    std::vector<bool> seen(inverse.size());
    for(size_t i=0;i<inverse.size();++i) {
        size_t index=order.empty() ? i : order[i];
        if(index>=inverse.size() || seen[index]) throw std::runtime_error("Animation triangle order is not a permutation");
        inverse[index]=i; seen[index]=true;
    }
    size_t expanded=0;
    for(auto ref:refs) {
        const auto &p=source.meshes[source.nodes[ref.node].mesh].primitives[ref.primitive];
        Primitive item; item.ref=ref; item.positions.resize(p.positions.size()); item.offsets.resize(p.positions.size()+1);
        for(auto i:p.indices) if(i<p.positions.size()) ++item.offsets[i+1];
        for(size_t i=1;i<item.offsets.size();++i) item.offsets[i]+=item.offsets[i-1];
        item.outputs.resize(item.offsets.back()); auto cursor=item.offsets;
        for(auto i:p.indices) if(i<p.positions.size()) {
            const size_t out=inverse[expanded/3]*3+expanded%3; ++expanded;
            item.outputs[cursor[i]++]=out;
            output[out]={};
            if(size_t(i)*2+1<p.uvs.size()) { output[out].uv[0]=p.uvs[size_t(i)*2]; output[out].uv[1]=p.uvs[size_t(i)*2+1]; }
        }
        primitives.push_back(std::move(item));
    }
    size_t total=0;
    for(const auto &p:primitives) total+=p.positions.size();
    const size_t workerCount=std::min(std::max(size_t(1),(total+8191)/8192),size_t(std::max(1u,std::thread::hardware_concurrency())));
    for(size_t i=0;i<workerCount;++i) {
        auto worker=std::make_unique<Worker>();
        const size_t begin=i*total/workerCount,end=(i+1)*total/workerCount;
        size_t base=0;
        for(size_t p=0;p<primitives.size();++p) {
            const size_t next=base+primitives[p].positions.size();
            if(begin<next && end>base) worker->ranges.push_back({p,std::max(begin,base)-base,std::min(end,next)-base});
            base=next;
        }
        workers.push_back(std::move(worker));
    }
    for(auto &worker:workers) worker->thread=std::thread([this,p=worker.get()] { Run(*p); });
}
void Animation::Run(Worker &w)
{
    size_t seen=0;
    for(;;) {
        std::unique_lock lock(mutex);
        wake.wait(lock,[&] { return stopping || generation!=seen; });
        if(stopping) return;
        seen=generation; lock.unlock(); Process(w);
        lock.lock(); if(--pending==0) done.notify_one();
    }
}
void Animation::Process(Worker &w)
{
    const bool seek=!reusePrevious && before!=now;
    for(auto range:w.ranges) {
        auto &item=primitives[range.primitive]; const auto ref=item.ref;
        const auto &p=source.meshes[source.nodes[ref.node].mesh].primitives[ref.primitive];
        const auto &current=currentPose.nodes[ref.node], &previous=previousPose.nodes[ref.node];
        for(size_t v=range.begin;v<range.end;++v) {
            Vec3 pos=item.positions[v],normal{},prev=pos;
            if(geometryChanged) DeformVertex(p,v,current.weights,current.joints,currentPose.worlds[ref.node],pos,normal);
            if(before==now) prev=pos;
            else if(seek) { Vec3 unused; DeformVertex(p,v,previous.weights,previous.joints,previousPose.worlds[ref.node],prev,unused); }
            item.positions[v]=pos;
            for(size_t k=item.offsets[v];k<item.offsets[v+1];++k) {
                auto &out=output[item.outputs[k]]; out.prevPos=prev;
                if(geometryChanged) { out.pos=pos; out.normal=normal; }
            }
        }
    }
}
bool Animation::Evaluate(float time,float previousTime)
{
    geometryChanged=false;
    if(workers.empty() || (evaluated && time==now && previousTime==before)) { cpuMs=0; return false; }
    const auto start=std::chrono::steady_clock::now();
    geometryChanged=!evaluated || time!=now;
    reusePrevious=evaluated && previousTime==now;
    if(geometryChanged) currentPose.Evaluate(source,time);
    if(!reusePrevious && previousTime!=time) previousPose.Evaluate(source,previousTime);
    std::unique_lock lock(mutex); now=time; before=previousTime; pending=workers.size(); ++generation;
    wake.notify_all(); done.wait(lock,[&] { return pending==0; });
    cpuMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
    evaluated=true; return true;
}
