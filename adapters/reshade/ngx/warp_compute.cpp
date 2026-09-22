#include "warp_compute.h"

#include "peripheral_warp/types_v2.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace pwhook {
namespace {

constexpr UINT kThreadsX = 16; // warp_*_cs.hlsl PW_WARP_THREADS_X / _Y
constexpr UINT kThreadsY = 8;

D3D12_STATIC_SAMPLER_DESC Sampler(UINT reg, D3D12_FILTER filter)
{
    // The adapter's static samplers (d3d12_adapter.cpp StaticSampler), visible to compute.
    D3D12_STATIC_SAMPLER_DESC s{};
    s.Filter = filter;
    s.AddressU = s.AddressV = s.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    s.MaxAnisotropy = 1;
    s.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    s.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
    s.MaxLOD = D3D12_FLOAT32_MAX;
    s.ShaderRegister = reg;
    s.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    return s;
}

bool UavWritable(ID3D12Resource *resource)
{
    return resource == nullptr || (resource->GetDesc().Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0;
}

void ColorAdjustBits(float gain, float gamma, std::uint32_t &gainBits, std::uint32_t &invGammaBits)
{
    // As D3D12Adapter::SetOutputColorAdjust: invalid values leave the adjustment unset (0 bits).
    gainBits = invGammaBits = 0;
    if (!std::isfinite(gain) || !std::isfinite(gamma) || gain <= 0.0f || gamma <= 0.0f) return;
    const float invGamma = 1.0f / gamma;
    std::memcpy(&gainBits, &gain, sizeof(gain));
    std::memcpy(&invGammaBits, &invGamma, sizeof(invGamma));
}

} // namespace

WarpCompute::~WarpCompute()
{
    if (constants_ && mapped_) constants_->Unmap(0, nullptr);
    for (IUnknown *o : std::initializer_list<IUnknown *>{constants_, heap_, unpackPso_, packPso_, rootSignature_, device_})
        if (o) o->Release();
}

bool WarpCompute::Initialize(ID3D12Device *device, const pwngx::Shaders &shaders, char *error, std::size_t errorSize)
{
    auto fail = [&](const char *what) {
        if (error && errorSize) std::snprintf(error, errorSize, "compute warp: %s", what);
        return false;
    };
    if (device == nullptr) return fail("no device");
    if (!shaders.WarpComputeLoaded()) return fail("shaders missing (optimizer-fps-dlss5\\warp_*_cs.dxbc)");
    device_ = device;
    device_->AddRef();

    // b0 warp constants, b1 input constants, t0..t3, b2 diagnostics, u0..u3, b3 dispatch size.
    D3D12_DESCRIPTOR_RANGE srv{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 4, 0, 0, 0};
    // Two tables (the UAVs are bound at their own handle, 4 descriptors in): both ranges start at 0.
    D3D12_DESCRIPTOR_RANGE uav{D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 4, 0, 0, 0};
    D3D12_ROOT_PARAMETER p[6]{};
    p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    p[0].Descriptor.ShaderRegister = 0;
    p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    p[1].Descriptor.ShaderRegister = 1;
    p[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    p[2].DescriptorTable = {1, &srv};
    p[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    p[3].Constants = {2, 0, 4};
    p[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    p[4].DescriptorTable = {1, &uav};
    p[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    p[5].Constants = {3, 0, 4};
    for (auto &param : p) param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    const D3D12_STATIC_SAMPLER_DESC samplers[] = {Sampler(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR),
                                                  Sampler(1, D3D12_FILTER_MIN_MAG_MIP_POINT)};
    D3D12_ROOT_SIGNATURE_DESC desc{6, p, 2, samplers, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ID3DBlob *blob = nullptr, *errors = nullptr;
    HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors);
    if (SUCCEEDED(hr)) hr = device_->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rootSignature_));
    if (blob) blob->Release();
    if (errors) errors->Release();
    if (FAILED(hr)) return fail("root signature");

    D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = rootSignature_;
    pso.CS = {shaders.warpPack.data(), shaders.warpPack.size()};
    if (FAILED(device_->CreateComputePipelineState(&pso, IID_PPV_ARGS(&packPso_)))) return fail("pack pipeline");
    pso.CS = {shaders.warpUnpack.data(), shaders.warpUnpack.size()};
    if (FAILED(device_->CreateComputePipelineState(&pso, IID_PPV_ARGS(&unpackPso_)))) return fail("unpack pipeline");

    D3D12_DESCRIPTOR_HEAP_DESC heap{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kRing * kTableSize,
                                    D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    if (FAILED(device_->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&heap_)))) return fail("descriptor heap");
    increment_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    D3D12_HEAP_PROPERTIES upload{};
    upload.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = static_cast<UINT64>(kRing) * kConstantSlot;
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(device_->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                nullptr, IID_PPV_ARGS(&constants_))))
        return fail("constant buffer");
    const D3D12_RANGE noRead{0, 0};
    if (FAILED(constants_->Map(0, &noRead, reinterpret_cast<void **>(&mapped_)))) return fail("constant buffer map");
    return true;
}

std::uint32_t WarpCompute::NextSlot()
{
    const std::uint32_t slot = next_;
    next_ = (next_ + 1) % kRing;
    return slot;
}

void WarpCompute::WriteConstants(std::uint32_t slot, const pw::LayoutV2 &layout, const pw::ShaderInputConstantsV2 &input)
{
    std::uint8_t *base = mapped_ + static_cast<std::size_t>(slot) * kConstantSlot;
    const pw::ShaderConstantsV2 warp = pw::BuildShaderConstants(layout);
    static_assert(sizeof(warp) <= 256 && sizeof(input) <= 256, "a constant slot holds two 256-byte buffers");
    std::memcpy(base, &warp, sizeof(warp));
    std::memcpy(base + 256, &input, sizeof(input));
}

void WarpCompute::WriteTable(std::uint32_t slot, const Views &srvs, const Views &uavs)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h = heap_->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(slot) * kTableSize * increment_;
    for (int i = 0; i < 4; ++i, h.ptr += increment_) {
        D3D12_SHADER_RESOURCE_VIEW_DESC d{};
        d.Format = srvs.formats[i];
        d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        d.Texture2D.MipLevels = 1;
        device_->CreateShaderResourceView(srvs.resources[i], &d, h); // a null resource is a null view
    }
    for (int i = 0; i < 4; ++i, h.ptr += increment_) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC d{};
        d.Format = uavs.formats[i];
        d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device_->CreateUnorderedAccessView(uavs.resources[i], nullptr, &d, h); // null: writes are discarded
    }
}

void WarpCompute::Dispatch(ID3D12GraphicsCommandList *cmd, ID3D12PipelineState *pso, std::uint32_t slot,
                           const std::uint32_t diagnostics[4], std::uint32_t width, std::uint32_t height)
{
    ID3D12DescriptorHeap *heaps[] = {heap_};
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->SetComputeRootSignature(rootSignature_);
    cmd->SetPipelineState(pso);
    const D3D12_GPU_VIRTUAL_ADDRESS cb = constants_->GetGPUVirtualAddress() + static_cast<UINT64>(slot) * kConstantSlot;
    cmd->SetComputeRootConstantBufferView(0, cb);
    cmd->SetComputeRootConstantBufferView(1, cb + 256);
    D3D12_GPU_DESCRIPTOR_HANDLE table = heap_->GetGPUDescriptorHandleForHeapStart();
    table.ptr += static_cast<UINT64>(slot) * kTableSize * increment_;
    cmd->SetComputeRootDescriptorTable(2, table);
    cmd->SetComputeRoot32BitConstants(3, 4, diagnostics, 0);
    table.ptr += 4ull * increment_;
    cmd->SetComputeRootDescriptorTable(4, table);
    const std::uint32_t size[4] = {width, height, 0, 0};
    cmd->SetComputeRoot32BitConstants(5, 4, size, 0);
    cmd->Dispatch((width + kThreadsX - 1) / kThreadsX, (height + kThreadsY - 1) / kThreadsY, 1);
    // Orders this pass's writes before whatever reads them without an intervening transition.
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    cmd->ResourceBarrier(1, &barrier);
}

pw::AdapterStatus WarpCompute::RecordPack(ID3D12GraphicsCommandList *cmd, const pw::LayoutV2 &layout,
                                          const pw::D3D12SourceResources &sources, const pw::InputDescriptionV2 &input,
                                          const pw::D3D12PackedViews &packed, DXGI_FORMAT packedColorView, std::uint32_t packSlot)
{
    if (cmd == nullptr || packSlot >= kPackSlots) return pw::AdapterStatus::InvalidArgument;
    pw::D3D12SourceValidation v{};
    const pw::AdapterStatus status = pw::ValidateD3D12Sources(device_, layout, sources, input, &v);
    if (status != pw::AdapterStatus::Ok) return status;
    if (!v.nativeExtent) return pw::AdapterStatus::ResourceMismatch;
    const pw::D3D12SourceResources &out = packed.resources;
    if (!UavWritable(out.color.resource) || !UavWritable(out.depth.resource) || !UavWritable(out.motion.resource) ||
        !UavWritable(out.confidence.resource))
        return pw::AdapterStatus::UnsupportedFormat; // the packed colour's format has no typed UAV store
    const std::uint32_t slot = NextSlot();
    WriteConstants(slot, layout, v.constants);
    const Views srvs{{sources.color.resource, sources.depth.resource, sources.motion.resource, sources.confidence.resource},
                     {v.viewFormats[0], v.viewFormats[1], v.viewFormats[2], v.viewFormats[3]}};
    const Views uavs{{out.color.resource, out.depth.resource, out.motion.resource, out.confidence.resource},
                     {packedColorView, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R16_FLOAT}};
    WriteTable(slot, srvs, uavs);
    const std::uint32_t diagnostics[4] = {};
    Dispatch(cmd, packPso_, slot, diagnostics, layout.workWidth, layout.workHeight);
    packConstants_[packSlot] = v.constants;
    packConstantsValid_[packSlot] = true;
    return pw::AdapterStatus::Ok;
}

pw::AdapterStatus WarpCompute::RecordUnpack(ID3D12GraphicsCommandList *cmd, const pw::LayoutV2 &layout,
                                            const pw::D3D12SourceResources &workSources, const pw::InputDescriptionV2 &input,
                                            ID3D12Resource *target, DXGI_FORMAT targetView, std::uint32_t outlines,
                                            float gain, float gamma)
{
    if (cmd == nullptr || target == nullptr) return pw::AdapterStatus::InvalidArgument;
    if (!UavWritable(target)) return pw::AdapterStatus::UnsupportedFormat;
    pw::D3D12SourceValidation v{};
    const pw::AdapterStatus status = pw::ValidateD3D12Sources(device_, layout, workSources, input, &v);
    if (status != pw::AdapterStatus::Ok) return status;
    if (!v.workExtent) return pw::AdapterStatus::ResourceMismatch;
    const std::uint32_t slot = NextSlot();
    WriteConstants(slot, layout, v.constants);
    const Views srvs{{workSources.color.resource, workSources.depth.resource, workSources.motion.resource, workSources.confidence.resource},
                     {v.viewFormats[0], v.viewFormats[1], v.viewFormats[2], v.viewFormats[3]}};
    const Views uavs{{target, nullptr, nullptr, nullptr}, {targetView, targetView, targetView, targetView}};
    WriteTable(slot, srvs, uavs);
    std::uint32_t diagnostics[4] = {outlines, 0, 0, 0};
    ColorAdjustBits(gain, gamma, diagnostics[1], diagnostics[2]);
    Dispatch(cmd, unpackPso_, slot, diagnostics, layout.nativeWidth, layout.nativeHeight);
    return pw::AdapterStatus::Ok;
}

pw::AdapterStatus WarpCompute::RecordUnpackPacked(ID3D12GraphicsCommandList *cmd, const pw::LayoutV2 &layout,
                                                  const pw::D3D12PackedViews &packed, DXGI_FORMAT packedColorView,
                                                  std::uint32_t packSlot, ID3D12Resource *target, DXGI_FORMAT targetView,
                                                  float gain, float gamma)
{
    if (cmd == nullptr || target == nullptr || packSlot >= kPackSlots || !packConstantsValid_[packSlot])
        return pw::AdapterStatus::InvalidArgument;
    if (!UavWritable(target)) return pw::AdapterStatus::UnsupportedFormat;
    const std::uint32_t slot = NextSlot();
    WriteConstants(slot, layout, packConstants_[packSlot]);
    const pw::D3D12SourceResources &in = packed.resources;
    const Views srvs{{in.color.resource, in.depth.resource, in.motion.resource, in.confidence.resource},
                     {packedColorView, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R16_FLOAT}};
    const Views uavs{{target, nullptr, nullptr, nullptr}, {targetView, targetView, targetView, targetView}};
    WriteTable(slot, srvs, uavs);
    std::uint32_t diagnostics[4] = {};
    ColorAdjustBits(gain, gamma, diagnostics[1], diagnostics[2]);
    Dispatch(cmd, unpackPso_, slot, diagnostics, layout.nativeWidth, layout.nativeHeight);
    return pw::AdapterStatus::Ok;
}

} // namespace pwhook
