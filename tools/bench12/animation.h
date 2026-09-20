#pragma once
#include "tri_vertex.h"
#include "animation_time.h"
#include "animation_pose.h"
#include <condition_variable>
#include <mutex>
#include <thread>
#include <memory>

class Animation {
public:
    pwgltf::Scene source;
    double cpuMs=0;
    bool geometryChanged=false;
    ~Animation();
    // Output stays mapped and alive until the workers have stopped. Empty order uses source order.
    void Start(TriVertex *output,const std::vector<unsigned> &order={});
    bool Evaluate(float time,float previousTime);
private:
    struct Primitive {
        pwgltf::PrimitiveRef ref;
        std::vector<size_t> offsets, outputs;
        std::vector<Vec3> positions;
    };
    struct Range { size_t primitive,begin,end; };
    struct Worker {
        std::vector<Range> ranges;
        std::thread thread;
    };
    TriVertex *output=nullptr;
    AnimationPose currentPose,previousPose;
    std::vector<Primitive> primitives;
    std::vector<std::unique_ptr<Worker>> workers;
    std::mutex mutex;
    std::condition_variable wake,done;
    size_t generation=0,pending=0;
    bool stopping=false,evaluated=false,reusePrevious=false;
    float now=0,before=0;
    void Run(Worker &worker);
    void Process(Worker &worker);
};
