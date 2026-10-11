#pragma once
// --render raster: Hi-Z occlusion culling of small instances (raster_cull.h) in the camera passes (depth/normal prepass, tube
// prepass, scene pass; never the shadow maps: an object behind a wall can still cast a visible shadow). The first camera pass
// draws the occluders (the flattened triangles, the dynamic ones included, and the large instances) into this frame's depth
// buffer; Record then builds a Hi-Z of it and a compute pass tests each small candidate's box against it (conservative: a
// corner in front of the near plane is visible; shaders/raster_occlusion.hlsl, hiz_math.h) and writes per run the indirect
// draw arguments and the visible instances, which every camera pass then draws with ExecuteIndirect. No latency: the occluders
// are this frame's, so nothing pops when the camera turns or cuts. --no-occlusion-culling: the small instances are drawn as
// the others. --cull-check: everything is drawn, and after the scene pass an occlusion query per culled candidate (depth EQUAL,
// the scene pass's vertex shader) counts its visible samples; any culled-but-visible instance is printed. --cull-debug: the
// culled ones are drawn tinted red over the scene pass. Docs: INSTANCING.md, "Culling small objects".
#include "raster_instances.h"
#include "scene.h"

class RasterOcclusion {
public:
    void Create(Device &device, const Scene &scene);
    // The render size changed (or the first frame): the Hi-Z is recreated.
    void EnsureTargets(Device &device, unsigned width, unsigned height);
    // Once per frame before any camera pass: collects the last frame's readback (statistics, --cull-check) and resets.
    void BeginFrame(const Scene &scene);
    // Inside the first camera pass, after its occluders: Hi-Z and the cull pass. `depth`: DEPTH_WRITE on entry and on return,
    // `depthSrv` its R32 view. Leaves the compute root and pipeline set: the caller sets its graphics pipeline again.
    void Record(Device &device, const Scene &scene, const RasterInstances &lists, const RasterInstances::List &camera,
                ID3D12Resource *depth, unsigned depthSrv, D3D12_GPU_VIRTUAL_ADDRESS frameConstants, bool reverse);
    bool Recorded() const { return recorded; }
    // The camera pass's small runs: the visible ones by ExecuteIndirect (--cull-check: all of them, directly), then rebinds the
    // instance lists' buffer.
    void DrawVisible(ID3D12GraphicsCommandList *list, const Scene &scene, const RasterInstances &lists, const RasterInstances::List &camera) const;
    // After the scene pass (raster_occlusion_check.cpp): --cull-check's queries, --cull-debug's overlay. `root`: the scene pass's
    // root signature (bound), `colour` the colour target's RTV, `dsv` the depth buffer's.
    void RecordCheck(Device &device, const Scene &scene, const RasterInstances::List &camera, ID3D12RootSignature *root,
                     D3D12_CPU_DESCRIPTOR_HANDLE dsv, bool reverse, const RasterInstances &lists);
    void RecordDebug(Device &device, ID3D12RootSignature *root, D3D12_CPU_DESCRIPTOR_HANDLE colour, const RasterInstances &lists);
    void Report() const; // the averages over the run
    bool check=false, debug=false;
private:
    struct Candidate { unsigned instance, run; };
    void CreatePipelines(Device &device);
    ID3D12PipelineState *CheckPso(Device &device, ID3D12RootSignature *root, bool reverse);
    ID3D12PipelineState *DebugPso(Device &device, ID3D12RootSignature *root);
    void CollectCheck(const Scene &scene);
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> hizPso, cullPso;
    ComPtr<ID3D12CommandSignature> drawSignature;
    ComPtr<ID3D12Resource> hiz, boxes, upload, args, visible, flags, readback;
    ComPtr<ID3D12QueryHeap> queries;
    struct RootPso { ID3D12RootSignature *root; bool reverse; ComPtr<ID3D12PipelineState> pso; };
    std::vector<RootPso> checkPsos, debugPsos;
    D3D12_VERTEX_BUFFER_VIEW visibleView{};
    std::vector<unsigned> levelWidth, levelHeight;
    unsigned descriptors=0, levels=0, width=0, height=0, smallInstances=0, maxRuns=0;
    unsigned runs=0, candidates=0; // this frame's
    Candidate *mappedCandidates=nullptr;
    unsigned *mappedArgs=nullptr;
    bool recorded=false, argsIndirect=false, pendingReadback=false, pendingCheck=false;
    std::vector<Candidate> lastCandidates; // the frame whose readback is pending
    std::vector<unsigned> lastRunInstances; // its candidates per run
    // Statistics over the run.
    unsigned frames=0;
    double candidateTotal=0, occludedTotal=0;
    unsigned checkedFrames=0, checkedCulled=0, violations=0;
};
