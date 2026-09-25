#include "core/frame/motion_smooth.h"

#include "core/frame/feature_state.h"
#include "core/context.h"
#include "core/frame/host_depth_state.h"

#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>

namespace ofps::core {
namespace {

constexpr std::uint32_t kRing = 64;          // descriptor sets: the host keeps several frames in flight
constexpr std::uint32_t kPerSet = 3;         // SRV motion, SRV depth, UAV output
constexpr float kDepthTolerance = 0.03f;     // relative mismatch at which a cell stops counting as this surface
constexpr std::uint32_t kGroupX = 16, kGroupY = 8;

// What the scope did over the last kReportEvery evaluates, logged so a game's log says whether the
// vectors were smoothed and, if not, why. Touched under the hook's mutex only.
struct Report {
    std::uint32_t applied = 0, noGrid = 0, keyOff = 0, noInputs = 0, failed = 0;
    int lastGrid = 0;
};
Report g_report;
constexpr std::uint64_t kReportEvery = 120;

void Account(Report &r)
{
    if (Ctx().evalCounter % kReportEvery != 0) return;
    Log(false, "Optimizer FPS: motion smoothing over the last %llu evaluates: applied %u (grid %d); passed through: "
               "no grid %u, DebugMotionSmooth off %u, no motion/depth %u, pass unavailable %u",
        static_cast<unsigned long long>(kReportEvery), r.applied, r.lastGrid, r.noGrid, r.keyOff, r.noInputs, r.failed);
    r = Report{};
}

std::vector<char> ReadShader(const std::wstring &directory)
{
    // The add-on's directory; its shaders sit in optimizer-fps-dlss5\ beside it, as LoadShaders reads them.
    std::ifstream file(directory + L"\\optimizer-fps-dlss5\\motion_smooth_cs.dxbc", std::ios::binary);
    return std::vector<char>((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

template <class T> void SafeRelease(T *&p)
{
    if (p != nullptr) p->Release();
    p = nullptr;
}

bool TypedUavStore(ID3D12Device *device, DXGI_FORMAT format)
{
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support{format, {}, {}};
    return SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support))) &&
           (support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) != 0;
}

} // namespace

MotionSmooth::~MotionSmooth()
{
    SafeRelease(output_);
    SafeRelease(heap_);
    SafeRelease(pipeline_);
    SafeRelease(rootSignature_);
}

bool MotionSmooth::Fits(const D3D12_RESOURCE_DESC &motion) const
{
    return output_ == nullptr ||
           (outputDesc_.Width == motion.Width && outputDesc_.Height == motion.Height &&
            outputDesc_.Format == TypedView(motion.Format, false));
}

bool MotionSmooth::Create(ID3D12Device *device, const D3D12_RESOURCE_DESC &motion)
{
    const std::vector<char> shader = ReadShader(Ctx().shaderDirectory);
    if (shader.empty()) {
        Log(true, "Optimizer FPS: motion_smooth_cs.dxbc is not beside the add-on; block-constant optical flow "
                  "vectors reach the model as they are");
        return false;
    }
    const DXGI_FORMAT format = TypedView(motion.Format, false);
    if (!TypedUavStore(device, format)) {
        Log(true, "Optimizer FPS: the host's motion format %u cannot be written by a compute pass; its vectors "
                  "reach the model as they are", static_cast<unsigned>(motion.Format));
        return false;
    }

    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 2;
    ranges[0].OffsetInDescriptorsFromTableStart = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 1;
    ranges[1].OffsetInDescriptorsFromTableStart = 2;
    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 2;
    params[0].DescriptorTable.pDescriptorRanges = ranges;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants.ShaderRegister = 0;
    params[1].Constants.Num32BitValues = 8;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rs{};
    rs.NumParameters = 2;
    rs.pParameters = params;
    ID3DBlob *blob = nullptr;
    ID3DBlob *error = nullptr;
    HRESULT hr = D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error);
    if (SUCCEEDED(hr))
        hr = device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rootSignature_));
    SafeRelease(blob);
    SafeRelease(error);
    if (FAILED(hr)) return false;

    D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = rootSignature_;
    pso.CS = {shader.data(), shader.size()};
    if (FAILED(device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&pipeline_)))) return false;

    D3D12_DESCRIPTOR_HEAP_DESC heap{};
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap.NumDescriptors = kRing * kPerSet;
    heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&heap_)))) return false;
    descriptorSize_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    if (!ofps::core::gpu::CreateTexture(device, static_cast<UINT>(motion.Width), motion.Height, format,
                              D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, &output_))
        return false;
    outputDesc_ = output_->GetDesc();
    outputState_ = D3D12_RESOURCE_STATE_COMMON;
    Log(false, "Optimizer FPS: block-constant optical flow vectors are smoothed within each surface before the "
               "model (%ux%u)", static_cast<unsigned>(motion.Width), motion.Height);
    return true;
}

ID3D12Resource *MotionSmooth::Record(ID3D12GraphicsCommandList *cmd, ID3D12Resource *motion, ID3D12Resource *depth,
                                     D3D12_RESOURCE_STATES depthState, UINT depthSub, D3D12_RESOURCE_STATES motionState, UINT motionSub, DXGI_FORMAT motionView, DXGI_FORMAT depthView, int grid)
{
    if (failed_) return nullptr;
    const D3D12_RESOURCE_DESC motionDesc = motion->GetDesc();
    const D3D12_RESOURCE_DESC depthDesc = depth->GetDesc();
    if (pipeline_ == nullptr) {
        // The list's own device: behind ReShade that is the proxy, which descriptor heaps must come from.
        ID3D12Device *device = nullptr;
        if (FAILED(cmd->GetDevice(IID_PPV_ARGS(&device))) || device == nullptr) { failed_ = true; return nullptr; }
        const bool ok = Create(device, motionDesc);
        device->Release();
        if (!ok) { failed_ = true; return nullptr; }
    }

    ID3D12Device *device = nullptr;
    heap_->GetDevice(IID_PPV_ARGS(&device));
    const std::uint32_t slot = nextSlot_++ % kRing;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = heap_->GetCPUDescriptorHandleForHeapStart();
    cpu.ptr += static_cast<SIZE_T>(slot) * kPerSet * descriptorSize_;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    srv.Format = motionView;
    device->CreateShaderResourceView(motion, &srv, cpu);
    cpu.ptr += descriptorSize_;
    srv.Format = depthView;
    srv.Texture2D.PlaneSlice = 0; // a planar depth-stencil guide is read through its depth plane
    device->CreateShaderResourceView(depth, &srv, cpu);
    cpu.ptr += descriptorSize_;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    uav.Format = outputDesc_.Format;
    device->CreateUnorderedAccessView(output_, nullptr, &uav, cpu);
    device->Release();

    BarrierExternal(cmd, motion, motionState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, motionSub);
    const bool depthReadable = (depthState & D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) != 0;
    if (!depthReadable)
        BarrierExternal(cmd, depth, depthState, depthState | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, depthSub);
    Barrier(cmd, output_, outputState_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    ID3D12DescriptorHeap *heaps[] = {heap_};
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->SetComputeRootSignature(rootSignature_);
    cmd->SetPipelineState(pipeline_);
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = heap_->GetGPUDescriptorHandleForHeapStart();
    gpu.ptr += static_cast<UINT64>(slot) * kPerSet * descriptorSize_;
    cmd->SetComputeRootDescriptorTable(0, gpu);
    std::uint32_t constants[8] = {static_cast<std::uint32_t>(motionDesc.Width), motionDesc.Height,
                                  static_cast<std::uint32_t>(depthDesc.Width), depthDesc.Height, 0, 0, 0, 0};
    const float cell = static_cast<float>(grid);
    std::memcpy(&constants[4], &cell, sizeof(float));
    const float tolerance = Ctx().diag.motionSmoothTolerance >= 0.0f ? Ctx().diag.motionSmoothTolerance : kDepthTolerance;
    std::memcpy(&constants[5], &tolerance, sizeof(float));
    cmd->SetComputeRoot32BitConstants(1, 8, constants, 0);
    cmd->Dispatch((static_cast<UINT>(motionDesc.Width) + kGroupX - 1) / kGroupX, (motionDesc.Height + kGroupY - 1) / kGroupY, 1);

    Barrier(cmd, output_, outputState_, kModelInputState);
    if (!depthReadable)
        BarrierExternal(cmd, depth, depthState | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, depthState, depthSub);
    BarrierExternal(cmd, motion, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, motionState, motionSub);
    return output_;
}

MotionSmoothScope::MotionSmoothScope(FeatureState &st, ID3D12GraphicsCommandList *cmd, const OfpsModelInputs &inputs)
    : st_(st)
{
    st.frameMotion = inputs.motion.res;
    const int grid = Ctx().blockMotionGrid.load();
    g_report.lastGrid = grid;
    struct Tally { ~Tally() { Account(g_report); } } tally;
    if (grid < 2) { ++g_report.noGrid; return; }
    if (!Ctx().diag.motionSmooth) { ++g_report.keyOff; return; }
    ID3D12Resource *motion = cmd != nullptr ? inputs.motion.res : nullptr;
    ID3D12Resource *depth = cmd != nullptr ? inputs.depth.res : nullptr;
    if (motion == nullptr || depth == nullptr) { ++g_report.noInputs; return; }
    if (st.motionSmooth && !st.motionSmooth->Fits(motion->GetDesc())) {
        // The host changed its motion texture (a resolution change): the old one may still be read by
        // frames in flight, so it goes to the graveyard and a fitting one is made below.
        ofps::core::gpu::Grave grave;
        grave.modelHost = st.modelHost;
        grave.gate = RetirementGate(st);
        grave.disposables.push_back(std::move(st.motionSmooth));
        Ctx().graveyard.Add(std::move(grave), Ctx().evalCounter);
    }
    if (!st.motionSmooth) st.motionSmooth = std::make_unique<MotionSmooth>();
    ID3D12Resource *smoothed = st.motionSmooth->Record(cmd, motion, depth, HostDepthState(inputs.depth),
                                                        inputs.depth.subresource, inputs.motion.restState, inputs.motion.subresource, inputs.motion.view, inputs.depth.view, grid);
    if (smoothed == nullptr) { ++g_report.failed; return; }
    ++g_report.applied;
    st.frameMotion = smoothed;
}

MotionSmoothScope::~MotionSmoothScope()
{
    st_.frameMotion = nullptr;
}

} // namespace ofps::core
