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
    bool windChanged=false; // the wind frame advanced: the wind vertices moved (they need an upload and a BLAS refit even if no character did)
    float windStrength=1; // --wind-strength; the wind itself is on for the nodes with Node::wind (set before Start)
    ~Animation();
    // Output stays mapped and alive until the workers have stopped. Empty order uses source order.
    void Start(TriVertex *output,const std::vector<unsigned> &order={});
    // `windFrame`: the bench frame index, the wind's clock (t = frame / 60 s), independent of the animation time.
    bool Evaluate(float time,float previousTime,unsigned windFrame=0) { return Evaluate(time,previousTime,windFrame,windFrame>0 ? windFrame-1 : 0); }
    // `windBefore`: the wind frame of the previous positions (windFrame - 1, or windFrame - K for --motion-span K).
    bool Evaluate(float time,float previousTime,unsigned windFrame,unsigned windBefore);
private:
    struct Primitive {
        pwgltf::PrimitiveRef ref;
        std::vector<size_t> offsets, outputs;
        std::vector<Vec3> positions;
        std::vector<float> wind; // WindWeights per vertex; empty unless the node has Node::wind
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
    bool stopping=false,evaluated=false,reusePrevious=false,hasWind=false;
    float now=0,before=0;
    unsigned windNow=0, windBefore=0;
    void Run(Worker &worker);
    void Process(Worker &worker);
};
