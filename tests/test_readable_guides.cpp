#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "hosts/reshade/readable_guides.h"
#include "hosts/reshade/ngx_params.h"
#include "hosts/reshade/shell_host.h"
#include "core/context.h"
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>
#include <iostream>
#include <map>
#include <set>
#include <string>

using Microsoft::WRL::ComPtr;
using namespace ofps::reshade;

namespace ofps::reshade {
IOfpsCore *Core() { return nullptr; }
bool DirectHostActive() { return false; }
ShellHost &Host() { static ShellHost host; return host; }
void ShellHost::Log(OfpsLogLevel, const char *text) { std::cout << text << '\n'; }
void ShellHost::OnEvent(OfpsEvent, const OfpsEventData *) {}
}

namespace {
int failures = 0;
void Check(bool ok, const char *what) { if (!ok) { std::cerr << "FAIL: " << what << '\n'; ++failures; } }
struct Block { void **table; std::map<std::string, std::uint64_t> values; };
void SetRaw(void *p, const char *key, std::uint64_t value) { static_cast<Block *>(p)->values[key] = value; }
int GetRaw(void *p, const char *key, std::uint64_t *value) {
    auto &values = static_cast<Block *>(p)->values;
    const auto it = values.find(key);
    if (it == values.end()) return 0;
    *value = it->second;
    return kNgxSuccess;
}
void *table[16] = {reinterpret_cast<void *>(&SetRaw), nullptr, nullptr, nullptr, nullptr,
                   nullptr, nullptr, nullptr, reinterpret_cast<void *>(&GetRaw)};

ComPtr<ID3D12Resource> Texture(ID3D12Device *device, DXGI_FORMAT format,
                               D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state,
                               const D3D12_CLEAR_VALUE *clear = nullptr, UINT16 mipLevels = 1)
{
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = 64; desc.Height = 64; desc.DepthOrArraySize = 1; desc.MipLevels = mipLevels;
    desc.Format = format; desc.SampleDesc.Count = 1; desc.Flags = flags;
    ComPtr<ID3D12Resource> result;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state,
        clear, IID_PPV_ARGS(&result)))) result.Reset();
    return result;
}

void Barrier(ID3D12GraphicsCommandList *cmd, ID3D12Resource *resource,
             D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after, UINT subresource)
{
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = {resource, subresource, before, after};
    cmd->ResourceBarrier(1, &b);
}
unsigned DebugErrors(ID3D12InfoQueue *messages)
{
    if (!messages) return 0;
    unsigned errors = 0;
    for (UINT64 i = 0; i < messages->GetNumStoredMessages(); ++i) {
        SIZE_T size = 0;
        messages->GetMessage(i, nullptr, &size);
        std::vector<std::uint8_t> bytes(size);
        auto *message = reinterpret_cast<D3D12_MESSAGE *>(bytes.data());
        if (SUCCEEDED(messages->GetMessage(i, message, &size)) &&
            message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
            std::cerr << "D3D12: " << message->pDescription << '\n';
            ++errors;
        }
    }
    return errors;
}
}

int main()
{
    const struct { DXGI_FORMAT source, depth, motion; } formats[] = {
        {DXGI_FORMAT_D24_UNORM_S8_UINT, DXGI_FORMAT_R24_UNORM_X8_TYPELESS, DXGI_FORMAT_R24_UNORM_X8_TYPELESS},
        {DXGI_FORMAT_R24G8_TYPELESS, DXGI_FORMAT_R24_UNORM_X8_TYPELESS, DXGI_FORMAT_R24_UNORM_X8_TYPELESS},
        {DXGI_FORMAT_D32_FLOAT_S8X24_UINT, DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS, DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS},
        {DXGI_FORMAT_R32G8X24_TYPELESS, DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS, DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS},
        {DXGI_FORMAT_D32_FLOAT, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_FLOAT},
        {DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_FLOAT},
        {DXGI_FORMAT_D16_UNORM, DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_R16_UNORM},
        {DXGI_FORMAT_R16_TYPELESS, DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_R16_FLOAT},
        {DXGI_FORMAT_R16G16_TYPELESS, DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R16G16_FLOAT},
        {DXGI_FORMAT_R32G32_TYPELESS, DXGI_FORMAT_R32G32_FLOAT, DXGI_FORMAT_R32G32_FLOAT},
    };
    for (const auto &f : formats) {
        Check(ReadableTwinFormat(f.source, true) == f.depth, "depth format mapping");
        Check(ReadableTwinFormat(f.source, false) == f.motion, "motion format mapping");
    }
    Check(ReadableTwinFormat(DXGI_FORMAT_R32_FLOAT, true) == DXGI_FORMAT_UNKNOWN, "readable format stays untouched");
    Check(DepthPlaneSubresource(DXGI_FORMAT_D24_UNORM_S8_UINT) == 0 &&
          DepthPlaneSubresource(DXGI_FORMAT_R24_UNORM_X8_TYPELESS) == 0, "plane 0 selection");

    ComPtr<ID3D12Debug> debug;
    const bool hasDebug = SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)));
    if (hasDebug) debug->EnableDebugLayer();
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter> warp;
    ComPtr<ID3D12Device> device;
    Check(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))), "DXGI factory");
    Check(factory && SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))), "WARP adapter");
    Check(warp && SUCCEEDED(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))), "WARP device");
    if (!device) return 1;
    ComPtr<ID3D12InfoQueue> messages;
    if (hasDebug) device.As(&messages);
    D3D12_CLEAR_VALUE clear{};
    clear.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    clear.DepthStencil.Depth = 0.375f;
    auto ds = Texture(device.Get(), DXGI_FORMAT_D24_UNORM_S8_UINT,
        D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL, D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear);
    auto host = Texture(device.Get(), DXGI_FORMAT_D24_UNORM_S8_UINT,
        D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    Check(ds && host, "typed depth textures");
    if (!ds || !host) return 1;
    D3D12_DESCRIPTOR_HEAP_DESC dsvDesc{}; dsvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV; dsvDesc.NumDescriptors = 1;
    ComPtr<ID3D12DescriptorHeap> dsv;
    Check(SUCCEEDED(device->CreateDescriptorHeap(&dsvDesc, IID_PPV_ARGS(&dsv))), "DSV heap");
    if (!dsv) return 1;
    device->CreateDepthStencilView(ds.Get(), nullptr, dsv->GetCPUDescriptorHandleForHeapStart());
    if (messages) messages->ClearStoredMessages();
    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> cmd;
    Check(SUCCEEDED(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue))), "queue");
    Check(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))), "allocator");
    Check(SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
        IID_PPV_ARGS(&cmd))), "list");
    if (!queue || !allocator || !cmd) return 1;
    cmd->ClearDepthStencilView(dsv->GetCPUDescriptorHandleForHeapStart(),
        D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 0.375f, 7, 0, nullptr);
    Barrier(cmd.Get(), ds.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_SOURCE,
        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);
    cmd->CopyResource(host.Get(), ds.Get());
    Barrier(cmd.Get(), host.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);

    Block block{table, {}};
    SetResource(&block, "DLSSNR.Depth", host.Get());
    ComPtr<ID3D12Resource> twin;
    std::set<ID3D12Resource *> copies;
    // No execute event or fence signals: repeated evaluates must stop allocating.
    for (unsigned i = 0; i < 7; ++i) {
        ReadableGuides guides(cmd.Get(), &block);
        auto *substitute = GetResource(&block, "DLSSNR.Depth");
        Check(substitute && substitute != host.Get(), "typed depth substituted");
        if (substitute) {
            Check(substitute->GetDesc().Format == DXGI_FORMAT_R24_UNORM_X8_TYPELESS, "readable twin format");
            copies.insert(substitute);
            twin = substitute;
        }
    }
    Check(copies.size() == 4, "pending twins capped at four without submission signals");
    Check(GetResource(&block, "DLSSNR.Depth") == host.Get(), "host pointer restored");
    auto mipMotion = Texture(device.Get(), DXGI_FORMAT_R16_TYPELESS,
        D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr, 2);
    Check(mipMotion != nullptr, "two-mip guide created");
    if (mipMotion) {
        SetResource(&block, "DLSSNR.MVec", mipMotion.Get());
        {
            ReadableGuides guides(cmd.Get(), &block);
            auto *substitute = GetResource(&block, "DLSSNR.MVec");
            Check(substitute && substitute != mipMotion.Get() && substitute->GetDesc().MipLevels == 1,
                  "two-mip source copied into one-mip twin");
        }
        Check(GetResource(&block, "DLSSNR.MVec") == mipMotion.Get(), "mip guide pointer restored");
        SetResource(&block, "DLSSNR.MVec", nullptr);
    }
    if (!twin) return 1;

    // Read the actual shell twin through an SRV, as the model does, then copy shader output to CPU.
    auto output = Texture(device.Get(), DXGI_FORMAT_R32_FLOAT,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    D3D12_DESCRIPTOR_HEAP_DESC viewDesc{};
    viewDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    viewDesc.NumDescriptors = 2;
    viewDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ComPtr<ID3D12DescriptorHeap> views;
    Check(output && SUCCEEDED(device->CreateDescriptorHeap(&viewDesc, IID_PPV_ARGS(&views))), "shader resources");
    if (!output || !views) return 1;
    auto srvHandle = views->GetCPUDescriptorHandleForHeapStart();
    auto uavHandle = srvHandle;
    uavHandle.ptr += device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(twin.Get(), &srv, srvHandle);
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = DXGI_FORMAT_R32_FLOAT;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    device->CreateUnorderedAccessView(output.Get(), nullptr, &uav, uavHandle);
    const char *shader = "Texture2D<float> d:register(t0); RWTexture2D<float> o:register(u0); "
                         "[numthreads(8,8,1)] void main(uint3 i:SV_DispatchThreadID){o[i.xy]=d[i.xy];}";
    ComPtr<ID3DBlob> code, error, signature;
    Check(SUCCEEDED(D3DCompile(shader, std::strlen(shader), nullptr, nullptr, nullptr, "main", "cs_5_0",
        0, 0, &code, &error)), "compile depth reader");
    if (!code) return 1;
    D3D12_DESCRIPTOR_RANGE ranges[2] = {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0},
                                          {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 1}};
    D3D12_ROOT_PARAMETER parameter{};
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameter.DescriptorTable = {2, ranges};
    D3D12_ROOT_SIGNATURE_DESC rootDesc{1, &parameter};
    Check(SUCCEEDED(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1,
        &signature, &error)), "root signature serialization");
    if (!signature) return 1;
    ComPtr<ID3D12RootSignature> root;
    Check(SUCCEEDED(device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(),
        IID_PPV_ARGS(&root))), "root signature");
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipelineDesc{};
    pipelineDesc.pRootSignature = root.Get();
    pipelineDesc.CS = {code->GetBufferPointer(), code->GetBufferSize()};
    ComPtr<ID3D12PipelineState> pipeline;
    Check(root && SUCCEEDED(device->CreateComputePipelineState(&pipelineDesc, IID_PPV_ARGS(&pipeline))), "compute pipeline");
    if (!pipeline) return 1;
    ID3D12DescriptorHeap *heaps[] = {views.Get()};
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->SetComputeRootSignature(root.Get());
    cmd->SetPipelineState(pipeline.Get());
    cmd->SetComputeRootDescriptorTable(0, views->GetGPUDescriptorHandleForHeapStart());
    cmd->Dispatch(8, 8, 1);

    D3D12_HEAP_PROPERTIES readHeap{}; readHeap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC buffer{}; buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = 64 * 256; buffer.Height = 1; buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1; buffer.SampleDesc.Count = 1; buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> readback;
    Check(SUCCEEDED(device->CreateCommittedResource(&readHeap, D3D12_HEAP_FLAG_NONE, &buffer,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback))), "readback");
    if (!readback) return 1;
    Barrier(cmd.Get(), output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_COPY_SOURCE, 0);
    D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
    src.pResource = output.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.pResource = readback.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint = {DXGI_FORMAT_R32_FLOAT, 64, 64, 1, 256};
    cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    Check(SUCCEEDED(cmd->Close()), "close list");
    Check(DebugErrors(messages.Get()) == 0, "recording debug layer clean");
    if (messages) messages->ClearStoredMessages();
    ID3D12CommandList *lists[] = {cmd.Get()}; queue->ExecuteCommandLists(1, lists);
    ReadableGuidesExecuted(queue.Get(), cmd.Get());
    ReadableGuidesPresented();
    ComPtr<ID3D12Fence> finish;
    Check(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&finish))), "finish fence");
    if (!finish) return 1;
    Check(SUCCEEDED(queue->Signal(finish.Get(), 1)), "finish signal");
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    Check(event && SUCCEEDED(finish->SetEventOnCompletion(1, event)) &&
          WaitForSingleObject(event, 5000) == WAIT_OBJECT_0, "GPU completion");
    if (event) CloseHandle(event);
    float *pixels = nullptr;
    D3D12_RANGE range{0, static_cast<SIZE_T>(buffer.Width)};
    Check(SUCCEEDED(readback->Map(0, &range, reinterpret_cast<void **>(&pixels))), "readback map");
    if (pixels) {
        const float first = pixels[0];
        const float last = pixels[63 * 64 + 63];
        Check(std::fabs(first - 0.375f) < 0.0001f && std::fabs(last - 0.375f) < 0.0001f,
              "copied depth plane reads as 0.375");
        readback->Unmap(0, nullptr);
    }
    if (messages) {
        Check(DebugErrors(messages.Get()) == 0, "debug layer clean on copy path");
        // The naive typed SRV is rejected by the debug layer when the layer is installed.
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc{}; heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heapDesc.NumDescriptors = 1;
        ComPtr<ID3D12DescriptorHeap> heap;
        if (SUCCEEDED(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&heap)))) {
            messages->ClearStoredMessages();
            D3D12_SHADER_RESOURCE_VIEW_DESC naiveSrv{}; naiveSrv.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
            naiveSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            naiveSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            naiveSrv.Texture2D.MipLevels = 1;
            device->CreateShaderResourceView(host.Get(), &naiveSrv, heap->GetCPUDescriptorHandleForHeapStart());
            Check(DebugErrors(messages.Get()) > 0, "naive typed SRV rejected");
        }
    }
    ShutdownReadableGuides();
    return failures ? 1 : 0;
}
