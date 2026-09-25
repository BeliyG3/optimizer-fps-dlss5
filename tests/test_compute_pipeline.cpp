#include "core/gpu/compute_pipeline.h"
#include "core/gpu/constant_ring.h"
#include "core/gpu/descriptor_pool.h"

#include <d3d12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>

using Microsoft::WRL::ComPtr;

int TestComputePipeline(ID3D12Device* device)
{
    int failures = 0;
    const auto check = [&](bool ok, const char* message) {
        if (!ok) {
            std::cerr << "FAIL: " << message << '\n';
            ++failures;
        }
    };

    // A queued recording must keep its own descriptor/CBV set until its fence passes.
    ComPtr<ID3D12Fence> fence;
    check(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))),
          "compute set ring: fence creation");
    if (!fence) return failures;
    ofps::core::gpu::SetRing ring;
    constexpr std::uint32_t kFirst = 10;
    constexpr std::uint32_t kSets = 64; // At least 32, with headroom for the observed queue.
    ring.Reset(kFirst, kSets);
    const OfpsFencePoint pending{sizeof(OfpsFencePoint), fence.Get(), 1};
    std::array<bool, kSets> seen{};
    for (std::uint32_t i = 0; i < kSets; ++i) {
        const auto set = ring.Acquire(pending, 0, 0);
        check(set >= kFirst && set < kFirst + kSets, "compute set ring: in-range set");
        if (set >= kFirst && set < kFirst + kSets) {
            check(!seen[set - kFirst], "compute set ring: no occupied set is overwritten");
            seen[set - kFirst] = true;
        }
    }
    check(ring.Acquire(pending, 0, 0) == ofps::core::gpu::DescriptorPool::kNone,
          "compute set ring: exhaustion leaves pending sets untouched");
    check(SUCCEEDED(fence->Signal(1)), "compute set ring: signal pending gate");
    const OfpsFencePoint next{sizeof(OfpsFencePoint), fence.Get(), 2};
    check(ring.Acquire(next, 0, 0) == kFirst, "compute set ring: reuse after signal");

    ofps::core::gpu::UploadBlocks constants;
    std::string reason;
    check(constants.Create(device, kSets * 3, reason), "compute constants: upload allocation");
    if (constants.GpuAddress(0)) {
        const std::array<std::uint32_t, 4> values{1, 2, 3, 4};
        check(constants.Write(3, values.data(), sizeof(values)), "compute constants: write block");
        check(!constants.Write(kSets * 3, values.data(), sizeof(values)),
              "compute constants: reject out-of-range block");
        check(constants.GpuAddress(3) - constants.GpuAddress(2) == ofps::core::gpu::UploadBlocks::kStride &&
                  constants.GpuAddress(3) % ofps::core::gpu::UploadBlocks::kStride == 0,
              "compute constants: each CBV starts on a 256-byte boundary");
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heapDesc.NumDescriptors = 1;
        ComPtr<ID3D12DescriptorHeap> heap;
        check(SUCCEEDED(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&heap))),
              "compute constants: descriptor heap");
        if (heap) {
            const auto handle = heap->GetCPUDescriptorHandleForHeapStart();
            check(constants.CreateView(device, 3, handle), "compute constants: CBV creation");
            check(!constants.CreateView(device, kSets * 3, handle),
                  "compute constants: no CBV for an exhausted set");
        }
    }

    constexpr char shaderSource[] =
        "RWTexture2D<float4> Output : register(u0);"
        "[numthreads(16, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {"
        " Output[id.xy] = float4(1, 0, 0, 1); }";
    ComPtr<ID3DBlob> shader, errors;
    const HRESULT compiled = D3DCompile(shaderSource, std::strlen(shaderSource), nullptr,
                                        nullptr, nullptr, "main", "cs_5_0", 0, 0,
                                        &shader, &errors);
    check(SUCCEEDED(compiled) && shader, "compute pipeline: test DXBC compiled");
    if (shader) {
        ofps::core::gpu::ComputePipeline ready;
        check(ready.Create(device, shader->GetBufferPointer(), shader->GetBufferSize(),
                           shader->GetBufferPointer(), shader->GetBufferSize()) && ready.Ready(),
              "compute pipeline: both named PSOs and root signature created");
        if (!ready.Ready()) std::cerr << ready.Reason() << '\n';
    }

    // Task 5 supplies real warp DXBC. For now, missing and truncated binaries must fail before
    // D3D12 sees an invalid PSO; both errors need a useful reason and a healthy device.
    constexpr char partial[] = {'D', 'X', 'B', 'C'};
    ofps::core::gpu::ComputePipeline missing;
    check(!missing.Create(device, nullptr, 0, partial, sizeof(partial)) &&
              missing.Reason().find("CSPack: missing or partial DXBC") != std::string::npos,
          "compute pipeline: missing Pack DXBC explains refusal");
    ofps::core::gpu::ComputePipeline truncated;
    check(!truncated.Create(device, partial, sizeof(partial), partial, sizeof(partial)) &&
              truncated.Reason().find("CSPack: missing or partial DXBC") != std::string::npos,
          "compute pipeline: partial Pack DXBC explains refusal");
    std::array<char, 32> header{};
    header[0] = 'D'; header[1] = 'X'; header[2] = 'B'; header[3] = 'C';
    header[24] = 64;
    ofps::core::gpu::ComputePipeline shortFile;
    check(!shortFile.Create(device, header.data(), header.size(), header.data(), header.size()) &&
              shortFile.Reason().find("partial DXBC") != std::string::npos,
          "compute pipeline: truncated declared DXBC size explains refusal");
    header[24] = 32;
    ofps::core::gpu::ComputePipeline missingUnpack;
    check(!missingUnpack.Create(device, header.data(), header.size(), nullptr, 0) &&
              missingUnpack.Reason().find("CSUnpack: missing or partial DXBC") != std::string::npos,
          "compute pipeline: missing Unpack DXBC explains refusal");
    check(device->GetDeviceRemovedReason() == S_OK,
          "compute pipeline: refused DXBC does not remove the device");
    return failures;
}
