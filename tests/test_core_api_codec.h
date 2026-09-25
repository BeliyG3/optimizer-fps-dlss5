#pragma once
#include "test_core_api_fakes.h"
#include <d3dcompiler.h>
namespace coretest {
// A reversible host codec, with a pre-uploaded colour/2 texture and a GPU decode.
// The reduced-grid variant decodes by nearest-neighbour enlargement into the frame.
struct CodecHost : FakeModelHost {
    ComPtr<ID3D12Resource> encoded;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pipeline;
    ComPtr<ID3D12DescriptorHeap> descriptors;
    OfpsResource before{}, answerSeen{};
    std::uint32_t prepareCalls = 0, resolveCalls = 0;
    ReadbackCapture answerCapture;
    bool Initialize(WarpDevice &w, std::uint32_t width, std::uint32_t height) {
        encoded = CreateTexture(w.device.Get(), width, height, kColorFormat, D3D12_RESOURCE_FLAG_NONE, kInputRest);
        if (!encoded || !BeginList(w))
            return false;
        std::vector<ComPtr<ID3D12Resource>> uploads;
        if (!UploadPattern(w, encoded.Get(), kInputRest, 0, uploads, 0.5f) || !SubmitList(w) ||
            !WaitForQueue(w.device.Get(), w.queue.Get()))
            return false;
        D3D12_DESCRIPTOR_RANGE ranges[2]{};
        ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[0].NumDescriptors = 1;
        ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[1].NumDescriptors = 1;
        D3D12_ROOT_PARAMETER params[2]{};
        for (int i = 0; i < 2; ++i) {
            params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            params[i].DescriptorTable = {1, &ranges[i]};
        }
        D3D12_ROOT_SIGNATURE_DESC desc{};
        desc.NumParameters = 2;
        desc.pParameters = params;
        ComPtr<ID3DBlob> signature, errors, shader;
        if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &errors)) ||
            FAILED(w.device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(),
                                                 IID_PPV_ARGS(&root))))
            return false;
        static constexpr char code[] = R"(
Texture2D<float4> answer : register(t0);
RWTexture2D<float4> output : register(u0);
[numthreads(8,8,1)] void main(uint3 id : SV_DispatchThreadID) {
    uint w,h,sw,sh; output.GetDimensions(w,h); answer.GetDimensions(sw,sh);
    if (id.x<w && id.y<h) output[id.xy]=answer.Load(int3(id.xy*uint2(sw,sh)/uint2(w,h),0))*2.0;
})";
        if (FAILED(D3DCompile(code, sizeof(code) - 1, nullptr, nullptr, nullptr, "main", "cs_5_0",
                              D3DCOMPILE_ENABLE_STRICTNESS, 0, &shader, &errors))) {
            if (errors)
                std::cerr << static_cast<const char *>(errors->GetBufferPointer());
            return false;
        }
        D3D12_COMPUTE_PIPELINE_STATE_DESC ps{};
        ps.pRootSignature = root.Get();
        ps.CS = {shader->GetBufferPointer(), shader->GetBufferSize()};
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = 2;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        return SUCCEEDED(w.device->CreateComputePipelineState(&ps, IID_PPV_ARGS(&pipeline))) &&
               SUCCEEDED(w.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&descriptors)));
    }
    int PrepareModelInput(ID3D12GraphicsCommandList *, const OfpsFrameInputs *frame, OfpsResource *color,
                          OfpsResource *frameBefore) override {
        ++prepareCalls;
        const auto desc = encoded->GetDesc();
        *color = Resource(encoded.Get(), kColorFormat, {0, 0, static_cast<UINT>(desc.Width), desc.Height}, kInputRest);
        before = *frameBefore = frame->color;
        return OFPS_OK;
    }
    int ResolveAnswer(ID3D12GraphicsCommandList *cmd, const OfpsResource *answer,
                      const OfpsFrameInputs *frame) override {
        ++resolveCalls;
        answerSeen = *answer;
        ComPtr<ID3D12Device> device;
        if (FAILED(cmd->GetDevice(IID_PPV_ARGS(&device))))
            return OFPS_E_DEVICE;
        answerCapture = CreateReadback(device.Get(), answer->res);
        if (!answerCapture.buffer)
            return OFPS_E_DEVICE;
        Transition(cmd, answer->res, answer->restState, D3D12_RESOURCE_STATE_COPY_SOURCE);
        RecordReadback(cmd, answer->res, answerCapture);
        Transition(cmd, answer->res, D3D12_RESOURCE_STATE_COPY_SOURCE, kInputRest);
        auto cpu = descriptors->GetCPUDescriptorHandleForHeapStart();
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = kColorFormat;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Texture2D.MipLevels = 1;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        device->CreateShaderResourceView(answer->res, &srv, cpu);
        const auto step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        cpu.ptr += step;
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = kColorFormat;
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(frame->output.res, nullptr, &uav, cpu);
        ID3D12DescriptorHeap *heaps[] = {descriptors.Get()};
        cmd->SetDescriptorHeaps(1, heaps);
        cmd->SetComputeRootSignature(root.Get());
        cmd->SetPipelineState(pipeline.Get());
        auto gpu = descriptors->GetGPUDescriptorHandleForHeapStart();
        cmd->SetComputeRootDescriptorTable(0, gpu);
        gpu.ptr += step;
        cmd->SetComputeRootDescriptorTable(1, gpu);
        cmd->Dispatch((frame->output.rect.w + 7) / 8, (frame->output.rect.h + 7) / 8, 1);
        D3D12_RESOURCE_BARRIER uavBarrier{};
        uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uavBarrier.UAV.pResource = frame->output.res;
        cmd->ResourceBarrier(1, &uavBarrier);
        if (answer->restState != kInputRest)
            Transition(cmd, answer->res, kInputRest, answer->restState);
        return OFPS_OK;
    }
};
} // namespace coretest
