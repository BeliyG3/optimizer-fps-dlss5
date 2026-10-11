#include "gpu_skinning.h"
#include "pipeline.h"
#include "skin_math.h"
#include <chrono>
#include <cstring>

namespace {
// shaders/skin.hlsl, cbuffer SkinPass (b0), as root constants.
struct SkinPass {
    unsigned vertexCount, dynamicBase, previousPalette, previousWeights;
    float windStrength; unsigned groupsX, padding[2];
    float windNow[4], windBefore[4];
};
static_assert(sizeof(SkinPass)==64);
constexpr unsigned GroupSize=64, MaxGroupsX=32768;
template<class T> ComPtr<ID3D12Resource> Upload(Device &d, const std::vector<T> &data)
{
    const T none{}; // a root SRV needs a buffer even when the table is empty
    return data.empty() ? d.UploadBuffer(&none,sizeof(T)) : d.UploadBuffer(data.data(),data.size()*sizeof(T));
}
D3D12_ROOT_PARAMETER RootView(D3D12_ROOT_PARAMETER_TYPE type, unsigned reg)
{
    D3D12_ROOT_PARAMETER p{}; p.ParameterType=type; p.Descriptor={reg,0}; p.ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL; return p;
}
void Phase(const SkinWindPhase &phase, float out[4]) { out[0]=phase.sinA; out[1]=phase.cosA; out[2]=phase.sinB; out[3]=phase.cosB; }
}

GpuSkinning::GpuSkinning(Device &d, const pwgltf::Scene &source, const std::vector<unsigned> &order, TriVertex *dynamicVertices,
                         unsigned base, float strength)
    : layout(BuildSkinLayout(source,order,dynamicVertices)), dynamicBase(base), windStrength(strength)
{
    const auto refs=pwgltf::Primitives(source,1);
    currentPose.Initialize(source,refs); previousPose.Initialize(source,refs);
    lastPalette.resize(layout.paletteEntries); lastWeights.resize(layout.weightCount);
    sources=Upload(d,layout.sources); items=Upload(d,layout.items); vertexItem=Upload(d,layout.vertexItem);
    offsets=Upload(d,layout.offsets); outputs=Upload(d,layout.outputs); targets=Upload(d,layout.targets); wind=Upload(d,layout.wind);
    const size_t paletteBytes=size_t(layout.paletteEntries)*2*sizeof(Affine3x4), weightBytes=std::max<size_t>(1,size_t(layout.weightCount)*2)*sizeof(float);
    frame=d.Buffer(paletteBytes+weightBytes,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
    void *mapped=nullptr; D3D12_RANGE empty{0,0}; Check(frame->Map(0,&empty,&mapped),"Map skinning palettes");
    mappedPalette=static_cast<Affine3x4 *>(mapped); mappedWeights=reinterpret_cast<float *>(static_cast<char *>(mapped)+paletteBytes);
    std::memset(mapped,0,paletteBytes+weightBytes);
    D3D12_ROOT_PARAMETER p[11]{};
    p[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; p[0].Constants={0,0,sizeof(SkinPass)/4};
    p[0].ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;
    for(unsigned i=0;i<9;++i) p[1+i]=RootView(D3D12_ROOT_PARAMETER_TYPE_SRV,i);
    p[10]=RootView(D3D12_ROOT_PARAMETER_TYPE_UAV,0);
    D3D12_ROOT_SIGNATURE_DESC rd{}; rd.NumParameters=UINT(std::size(p)); rd.pParameters=p;
    root=Root(d,rd);
    const auto cs=Shader("skin.cso");
    D3D12_COMPUTE_PIPELINE_STATE_DESC c{}; c.pRootSignature=root.Get(); c.CS={cs.data(),cs.size()};
    Check(d.gpu->CreateComputePipelineState(&c,IID_PPV_ARGS(&pso)),"Create GPU skinning PSO");
    size_t shared=0; for(const auto &item:layout.items) shared+=item.vertexCount;
    std::printf("[info] gpu skinning: %zu items on %zu nodes, %u vertices (%zu source vertices shared), %u corners, %u palette entries, "
                "%zu morph deltas%s\n",layout.items.size(),layout.nodes.size(),layout.ItemVertices(),shared-layout.sources.size(),
                layout.corners,layout.paletteEntries,layout.targets.size(),layout.wind.empty() ? "" : ", wind");
}
bool GpuSkinning::Evaluate(const pwgltf::Scene &source, float time, float previousTime, unsigned windFrame, unsigned windFrameBefore)
{
    geometryChanged=windChanged=false;
    const bool windMoves=!layout.wind.empty() && (!evaluated || windFrame!=windNow);
    if(layout.items.empty() || (evaluated && time==now && previousTime==before && !windMoves)) { cpuMs=0; return false; }
    const auto start=std::chrono::steady_clock::now();
    geometryChanged=!evaluated || time!=now; windChanged=windMoves;
    const unsigned entries=layout.paletteEntries, weights=layout.weightCount;
    Affine3x4 *previousPalette=mappedPalette+entries; float *previousWeights=mappedWeights+weights;
    // The previous frame's palette: the last current one when time steps on (Animation's reusePrevious), else evaluated.
    if(previousTime!=time) {
        if(evaluated && previousTime==now) {
            std::memcpy(previousPalette,lastPalette.data(),entries*sizeof(Affine3x4));
            if(weights) std::memcpy(previousWeights,lastWeights.data(),weights*sizeof(float));
        } else {
            previousPose.Evaluate(source,previousTime);
            std::vector<Affine3x4> rows(entries); std::vector<float> w(weights);
            WritePalette(layout,previousPose,rows.data()); WriteMorphWeights(layout,previousPose,w.data());
            std::memcpy(previousPalette,rows.data(),entries*sizeof(Affine3x4));
            if(weights) std::memcpy(previousWeights,w.data(),weights*sizeof(float));
        }
    }
    if(geometryChanged) {
        currentPose.Evaluate(source,time);
        WritePalette(layout,currentPose,lastPalette.data()); WriteMorphWeights(layout,currentPose,lastWeights.data());
    }
    std::memcpy(mappedPalette,lastPalette.data(),entries*sizeof(Affine3x4));
    if(weights) std::memcpy(mappedWeights,lastWeights.data(),weights*sizeof(float));
    if(previousTime==time) {
        std::memcpy(previousPalette,lastPalette.data(),entries*sizeof(Affine3x4));
        if(weights) std::memcpy(previousWeights,lastWeights.data(),weights*sizeof(float));
    }
    now=time; before=previousTime; windNow=windFrame; windBefore=windFrameBefore; evaluated=true;
    cpuMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
    return true;
}
void GpuSkinning::Record(Device &d, ID3D12Resource *vertices) const
{
    const unsigned count=layout.ItemVertices();
    if(!count) return;
    auto *list=d.list.Get();
    Transition(list,vertices,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    const unsigned groups=(count+GroupSize-1)/GroupSize, groupsX=std::min(groups,MaxGroupsX), groupsY=(groups+groupsX-1)/groupsX;
    SkinPass pass{}; pass.vertexCount=count; pass.dynamicBase=dynamicBase; pass.previousPalette=layout.paletteEntries;
    pass.previousWeights=layout.weightCount; pass.windStrength=windStrength; pass.groupsX=groupsX;
    Phase(WindPhase(windNow),pass.windNow); Phase(WindPhase(windBefore),pass.windBefore);
    list->SetComputeRootSignature(root.Get()); list->SetPipelineState(pso.Get());
    list->SetComputeRoot32BitConstants(0,sizeof(SkinPass)/4,&pass,0);
    ID3D12Resource *tables[]={sources.Get(),items.Get(),vertexItem.Get(),offsets.Get(),outputs.Get(),targets.Get(),wind.Get()};
    for(unsigned i=0;i<7;++i) list->SetComputeRootShaderResourceView(1+i,tables[i]->GetGPUVirtualAddress());
    const size_t paletteBytes=size_t(layout.paletteEntries)*2*sizeof(Affine3x4);
    list->SetComputeRootShaderResourceView(8,frame->GetGPUVirtualAddress());
    list->SetComputeRootShaderResourceView(9,frame->GetGPUVirtualAddress()+paletteBytes);
    list->SetComputeRootUnorderedAccessView(10,vertices->GetGPUVirtualAddress());
    list->Dispatch(groupsX,groupsY,1);
    // The transition back orders the writes before the BLAS refit and every read of the scene vertices.
    Transition(list,vertices,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
}
