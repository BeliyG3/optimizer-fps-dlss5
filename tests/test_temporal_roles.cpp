// VRAM step 1, temporal machine roles on WARP: a Hidden machine (one residual, no history, no
// phase-in textures) records the same residual and reprojection as a Synchronous one when nothing is
// blended, which is how the spread passes drive their hidden stages, and it never blends at all (the
// Synchronous machine's blend does change this scene, so the comparison can tell).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "core/gpu/barriers.h"
#include "core/temporal/machine.h"

#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace ofps::core::temporal;
using ofps::core::gpu::BarrierExternal;

namespace {
constexpr std::uint32_t kSize = 32;
constexpr auto kColor = DXGI_FORMAT_R16G16B16A16_FLOAT;
constexpr auto kRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
int failures = 0;

void Check(bool condition, const char *message)
{
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
}

ComPtr<ID3D12Resource> Texture(ID3D12Device *device, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags)
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = kSize;
    desc.Height = kSize;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Flags = flags;
    ComPtr<ID3D12Resource> texture;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON,
                                               nullptr, IID_PPV_ARGS(&texture))))
        texture.Reset();
    return texture;
}

// Clears `texture` to `base` with `patch` in a corner block, then leaves it in `rest`.
void Paint(ID3D12Device *device, ID3D12GraphicsCommandList *list, ID3D12DescriptorHeap *heap, UINT index,
           ID3D12Resource *texture, const float (&base)[4], const float (&patch)[4], D3D12_RESOURCE_STATES rest)
{
    auto rtv = heap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(index) * device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    device->CreateRenderTargetView(texture, nullptr, rtv);
    BarrierExternal(list, texture, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
    list->ClearRenderTargetView(rtv, base, 0, nullptr);
    const D3D12_RECT block{5, 7, 19, 23};
    list->ClearRenderTargetView(rtv, patch, 1, &block);
    BarrierExternal(list, texture, D3D12_RESOURCE_STATE_RENDER_TARGET, rest);
}

struct Scene {
    ComPtr<ID3D12Resource> color, depth, motion, fresh[2];
};

// Two full passes with a reprojected frame after each, as a spread stage sees them; the second frame
// goes into `output`. False when the descriptor ring ran out.
bool RecordPasses(Machine &machine, ID3D12GraphicsCommandList *list, const Scene &scene, ID3D12Resource *output,
                  float residualBlend)
{
    FrameInputs in{};
    in.color = scene.color.Get(); in.depth = scene.depth.Get(); in.motion = scene.motion.Get();
    in.colorView = kColor; in.depthView = DXGI_FORMAT_R32_FLOAT; in.motionView = DXGI_FORMAT_R16G16_FLOAT;
    in.colorRect = in.depthRect = in.motionRect = {0, 0, kSize, kSize};
    in.hostInputState = kRead;
    in.depthState = kRead;
    in.motionState = kRead;
    in.phaseInFrames = 0;
    in.residualBlend = residualBlend;
    for (int pass = 0; pass < 2; ++pass) {
        machine.RecordResidual(list, in, scene.fresh[pass].Get(), kRead);
        machine.RecordAccumulate(list, in);
        machine.RecordReproject(list, in, pass == 1 ? output : nullptr, D3D12_RESOURCE_STATE_COMMON, 0, 0);
    }
    return !machine.TakeExhausted();
}
} // namespace

int TestTemporalRoles(ID3D12Device *device, ID3D12CommandQueue *queue, const char *shaderDirectory)
{
    static_assert(!TexturesFor(MachineRole::Hidden).previousResidual && !TexturesFor(MachineRole::Hidden).history);
    ofps::core::gpu::Shaders shaders;
    auto read = [&](const char *name, std::vector<char> &code) {
        std::ifstream file(std::filesystem::path(shaderDirectory) / name, std::ios::binary);
        code.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    };
    read("fullscreen_vs.dxbc", shaders.vertex);
#define PW_TEMPORAL_PASS(name, member, reads, outputs, extent) read("temporal_" #name "_cs.dxbc", shaders.temporal##name);
#include "core/shaders/temporal_passes.def"
#undef PW_TEMPORAL_PASS
    read("temporal_RefineModel_cs.dxbc", shaders.temporalRefineModel);
    read("temporal_FlowLumaModel_cs.dxbc", shaders.temporalFlowLumaModel);

    constexpr auto kRt = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    Scene scene;
    scene.color = Texture(device, kColor, kRt);
    scene.depth = Texture(device, DXGI_FORMAT_R32_FLOAT, kRt);
    scene.motion = Texture(device, DXGI_FORMAT_R16G16_FLOAT, kRt);
    scene.fresh[0] = Texture(device, kColor, kRt);
    scene.fresh[1] = Texture(device, kColor, kRt);
    constexpr UINT kRuns = 4;
    ComPtr<ID3D12Resource> outputs[kRuns];
    for (auto &output : outputs) output = Texture(device, kColor, D3D12_RESOURCE_FLAG_NONE);
    ComPtr<ID3D12DescriptorHeap> heap;
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heapDesc.NumDescriptors = 5;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    Check(scene.color && scene.depth && scene.motion && scene.fresh[0] && scene.fresh[1] && outputs[0] && outputs[1] &&
              outputs[2] && outputs[3] && SUCCEEDED(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&heap))) &&
              SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) &&
              SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                                  IID_PPV_ARGS(&list))) &&
              SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))),
          "roles: fixtures allocate");
    if (failures) return failures;

    Paint(device, list.Get(), heap.Get(), 0, scene.color.Get(), {0.4f, 0.3f, 0.2f, 1.0f}, {0.8f, 0.1f, 0.6f, 1.0f}, kRead);
    Paint(device, list.Get(), heap.Get(), 1, scene.depth.Get(), {0.5f, 0, 0, 0}, {0.25f, 0, 0, 0}, kRead);
    Paint(device, list.Get(), heap.Get(), 2, scene.motion.Get(), {0.0f, 0.0f, 0, 0}, {1.5f, -0.5f, 0, 0}, kRead);
    Paint(device, list.Get(), heap.Get(), 3, scene.fresh[0].Get(), {0.5f, 0.3f, 0.1f, 1.0f}, {0.9f, 0.2f, 0.5f, 1.0f}, kRead);
    Paint(device, list.Get(), heap.Get(), 4, scene.fresh[1].Get(), {0.45f, 0.35f, 0.15f, 1.0f}, {0.7f, 0.25f, 0.65f, 1.0f}, kRead);

    // [0] a Synchronous machine, [1] a Hidden one, [2] a Hidden one asked to blend: all must agree.
    // [3] a Synchronous machine that blends: must differ, or the scene could not show a blend.
    const struct { MachineRole role; float blend; } runs[kRuns] = {
        {MachineRole::Synchronous, 0.0f}, {MachineRole::Hidden, 0.0f}, {MachineRole::Hidden, 0.6f},
        {MachineRole::Synchronous, 0.6f}};
    Machine machines[kRuns];
    for (UINT i = 0; i < kRuns; ++i) {
        char error[256]{};
        Check(machines[i].Initialize(device, shaders, kSize, kSize, kColor, kColor, kSize, kSize, DXGI_FORMAT_R32_FLOAT,
                                     kSize, kSize, error, sizeof(error), 3, runs[i].role),
              "roles: machine initializes");
        if (!machines[i].Ready()) { std::cerr << error << '\n'; return failures; }
        Check(machines[i].Role() == runs[i].role &&
                  machines[i].Matches(kSize, kSize, kColor, kSize, kSize, DXGI_FORMAT_R32_FLOAT, kSize, kSize, 3, runs[i].role) &&
                  !machines[i].Matches(kSize, kSize, kColor, kSize, kSize, DXGI_FORMAT_R32_FLOAT, kSize, kSize, 3,
                                       MachineRole::Background),
              "roles: the role is part of Matches");
        OfpsFencePoint point{};
        point.size = sizeof(point);
        point.fence = fence.Get();
        point.value = 1;
        machines[i].SetUsePoint(point, 0);
        Check(RecordPasses(machines[i], list.Get(), scene, outputs[i].Get(), runs[i].blend),
              "roles: two passes record within the descriptor ring");
    }

    // Read the reprojections back and compare them byte for byte.
    constexpr UINT kPitch = 256;
    D3D12_HEAP_PROPERTIES props{};
    props.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = static_cast<UINT64>(kRuns) * kPitch * kSize;
    buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> readback;
    Check(SUCCEEDED(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST,
                                                   nullptr, IID_PPV_ARGS(&readback))),
          "roles: readback allocates");
    if (!readback) return failures;
    for (UINT i = 0; i < kRuns; ++i) {
        BarrierExternal(list.Get(), outputs[i].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
        dst.pResource = readback.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Offset = static_cast<UINT64>(i) * kPitch * kSize;
        dst.PlacedFootprint.Footprint = {kColor, kSize, kSize, 1, kPitch};
        src.pResource = outputs[i].Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    Check(SUCCEEDED(list->Close()), "roles: list closes");
    ID3D12CommandList *lists[] = {list.Get()};
    queue->ExecuteCommandLists(1, lists);
    HANDLE done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    const bool finished = done && SUCCEEDED(queue->Signal(fence.Get(), 1)) &&
                          SUCCEEDED(fence->SetEventOnCompletion(1, done)) && WaitForSingleObject(done, 10000) == WAIT_OBJECT_0;
    if (done) CloseHandle(done);
    Check(finished && device->GetDeviceRemovedReason() == S_OK, "roles: the GPU completes the passes");
    if (!finished) return failures;
    void *data = nullptr;
    const D3D12_RANGE all{0, static_cast<SIZE_T>(buffer.Width)};
    Check(SUCCEEDED(readback->Map(0, &all, &data)), "roles: readback maps");
    if (!data) return failures;
    const auto *bytes = static_cast<const unsigned char *>(data);
    const std::size_t image = static_cast<std::size_t>(kPitch) * kSize;
    Check(std::memcmp(bytes, bytes + image, image) == 0, "roles: a Hidden machine reprojects bit-identically to a Synchronous one");
    Check(std::memcmp(bytes + image, bytes + 2 * image, image) == 0, "roles: a Hidden machine never blends its residual");
    Check(std::memcmp(bytes, bytes + 3 * image, image) != 0, "roles: a Synchronous machine's blend changes this scene");
    const D3D12_RANGE none{0, 0};
    readback->Unmap(0, &none);
    return failures;
}
