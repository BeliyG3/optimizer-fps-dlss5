#pragma once
#include "animation.h"

// Benchmark-only copy of the original primitive-partitioned evaluation/copy pipeline.
// Keep this independent of Animation so future optimizations retain a useful baseline.
class AnimationBaseline {
    struct Worker {
        std::vector<pwgltf::PrimitiveRef> refs;
        pwgltf::VertexWorkspace scratch;
        std::vector<pwgltf::Vertex> vertices;
        std::vector<Vec3> previous;
        size_t offset=0;
        std::thread thread;
    };
    const pwgltf::Scene &source;
    std::vector<std::unique_ptr<Worker>> workers;
    std::vector<pwgltf::Vertex> vertices;
    std::vector<Vec3> previous;
    std::mutex mutex;
    std::condition_variable wake,done;
    size_t generation=0,pending=0;
    bool stop=false;
    float now=0,before=0;
public:
    explicit AnimationBaseline(const pwgltf::Scene &scene):source(scene) {
        auto refs=pwgltf::Primitives(source,1);
        const size_t count=std::min(refs.size(),size_t(std::max(1u,std::thread::hardware_concurrency())));
        size_t total=0;
        for(size_t i=0;i<count;++i) {
            auto w=std::make_unique<Worker>();
            w->refs.assign(refs.begin()+i*refs.size()/count,refs.begin()+(i+1)*refs.size()/count);
            w->scratch.Reserve(source,w->refs);
            pwgltf::BuildVerticesSubset(source,0,0,w->refs,w->scratch,w->vertices,w->previous);
            w->offset=total; total+=w->vertices.size(); workers.push_back(std::move(w));
        }
        vertices.resize(total); previous.resize(total);
        for(auto &w:workers) w->thread=std::thread([this,p=w.get()] {
            size_t seen=0;
            for(;;) {
                std::unique_lock lock(mutex); wake.wait(lock,[&] { return stop || generation!=seen; });
                if(stop) return;
                seen=generation; lock.unlock();
                pwgltf::BuildVerticesSubset(source,now,0,p->refs,p->scratch,p->vertices,p->previous,nullptr,nullptr,before);
                std::copy(p->vertices.begin(),p->vertices.end(),vertices.begin()+p->offset);
                std::copy(p->previous.begin(),p->previous.end(),previous.begin()+p->offset);
                lock.lock(); if(--pending==0) done.notify_one();
            }
        });
    }
    ~AnimationBaseline() {
        { std::lock_guard lock(mutex); stop=true; }
        wake.notify_all(); for(auto &w:workers) w->thread.join();
    }
    void Evaluate(float time,float previousTime,TriVertex *output,const std::vector<unsigned> &order) {
        if(workers.empty()) return;
        std::unique_lock lock(mutex); now=time; before=previousTime; pending=workers.size(); ++generation;
        wake.notify_all(); done.wait(lock,[&] { return pending==0; }); lock.unlock();
        for(size_t triangle=0;triangle<order.size();++triangle) for(size_t k=0;k<3;++k) {
            const size_t index=size_t(order[triangle])*3+k; const auto &v=vertices[index];
            output[triangle*3+k]={v.pos,previous[index],v.normal,{v.uv[0],v.uv[1]}};
        }
    }
};
