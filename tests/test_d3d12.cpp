#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "d3d12_adapter.h"
#include "peripheral_warp/math.h"

#include <d3d12sdklayers.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

int failures = 0;

void Check(bool condition, const char *message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

bool Near(float actual, float expected, float tolerance = 0.02f)
{
    return std::abs(actual - expected) <= tolerance;
}

std::vector<char> ReadBinary(const char *path)
{
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) return {};
    const std::streamsize size = stream.tellg();
    if (size <= 0) return {};
    std::vector<char> bytes(static_cast<std::size_t>(size));
    stream.seekg(0);
    stream.read(bytes.data(), size);
    return stream ? bytes : std::vector<char>{};
}

ComPtr<ID3D12Resource> CreateTexture(
    ID3D12Device *device,
    std::uint32_t width,
    std::uint32_t height,
    DXGI_FORMAT format,
    D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE,
    D3D12_RESOURCE_STATES initialState = D3D12_RESOURCE_STATE_COMMON)
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap.CreationNodeMask = 1;
    heap.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = flags;
    ComPtr<ID3D12Resource> result;
    if (FAILED(device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &desc, initialState,
            nullptr, IID_PPV_ARGS(&result))))
        result.Reset();
    return result;
}

struct ReadbackCapture {
    ComPtr<ID3D12Resource> buffer;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 byteCount = 0;
};

ReadbackCapture CreateReadback(ID3D12Device *device, ID3D12Resource *source)
{
    ReadbackCapture result;
    if (device == nullptr || source == nullptr) return result;

    const D3D12_RESOURCE_DESC sourceDesc = source->GetDesc();
    UINT rows = 0;
    UINT64 rowBytes = 0;
    device->GetCopyableFootprints(
        &sourceDesc, 0, 1, 0, &result.footprint, &rows, &rowBytes, &result.byteCount);
    if (result.byteCount == 0) return {};

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    heap.CreationNodeMask = 1;
    heap.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = result.byteCount;
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.Format = DXGI_FORMAT_UNKNOWN;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &buffer,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&result.buffer))))
        return {};
    return result;
}

void Transition(ID3D12GraphicsCommandList *list, ID3D12Resource *resource,
                D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    list->ResourceBarrier(1, &barrier);
}

void RecordReadback(ID3D12GraphicsCommandList *list, ID3D12Resource *source,
                    const ReadbackCapture &readback)
{
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = readback.buffer.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint = readback.footprint;
    D3D12_TEXTURE_COPY_LOCATION sourceLocation{};
    sourceLocation.pResource = source;
    sourceLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    sourceLocation.SubresourceIndex = 0;
    list->CopyTextureRegion(&destination, 0, 0, 0, &sourceLocation, nullptr);
}

bool ReadPixelBytes(const ReadbackCapture &capture, std::uint32_t x, std::uint32_t y,
                    void *output, std::size_t byteCount)
{
    if (!capture.buffer || output == nullptr ||
        x >= capture.footprint.Footprint.Width || y >= capture.footprint.Footprint.Height)
        return false;
    const UINT64 offset = capture.footprint.Offset +
        static_cast<UINT64>(y) * capture.footprint.Footprint.RowPitch +
        static_cast<UINT64>(x) * byteCount;
    if (offset + byteCount > capture.byteCount) return false;

    const D3D12_RANGE readRange{static_cast<SIZE_T>(offset),
                                static_cast<SIZE_T>(offset + byteCount)};
    void *mapped = nullptr;
    if (FAILED(capture.buffer->Map(0, &readRange, &mapped))) return false;
    std::memcpy(output, static_cast<const std::byte *>(mapped) + offset, byteCount);
    const D3D12_RANGE noWrite{0, 0};
    capture.buffer->Unmap(0, &noWrite);
    return true;
}

float HalfToFloat(std::uint16_t value)
{
    const float sign = (value & 0x8000u) != 0 ? -1.0f : 1.0f;
    const std::uint32_t exponent = (value >> 10) & 0x1fu;
    const std::uint32_t mantissa = value & 0x03ffu;
    if (exponent == 0)
        return sign * std::ldexp(static_cast<float>(mantissa), -24);
    if (exponent == 0x1fu)
        return mantissa == 0 ? sign * std::numeric_limits<float>::infinity()
                             : std::numeric_limits<float>::quiet_NaN();
    return sign * std::ldexp(static_cast<float>(1024u + mantissa),
                             static_cast<int>(exponent) - 25);
}

bool ReadHalf2(const ReadbackCapture &capture, std::uint32_t x, std::uint32_t y,
               std::array<float, 2> *result)
{
    std::array<std::uint16_t, 2> raw{};
    if (result == nullptr || !ReadPixelBytes(capture, x, y, raw.data(), sizeof(raw))) return false;
    *result = {HalfToFloat(raw[0]), HalfToFloat(raw[1])};
    return true;
}

bool ReadHalf4(const ReadbackCapture &capture, std::uint32_t x, std::uint32_t y,
               std::array<float, 4> *result)
{
    std::array<std::uint16_t, 4> raw{};
    if (result == nullptr || !ReadPixelBytes(capture, x, y, raw.data(), sizeof(raw))) return false;
    *result = {HalfToFloat(raw[0]), HalfToFloat(raw[1]),
               HalfToFloat(raw[2]), HalfToFloat(raw[3])};
    return true;
}

bool ReadHalf1(const ReadbackCapture &capture, std::uint32_t x, std::uint32_t y,
               float *result)
{
    std::uint16_t raw = 0;
    if (result == nullptr || !ReadPixelBytes(capture, x, y, &raw, sizeof(raw))) return false;
    *result = HalfToFloat(raw);
    return true;
}

bool ReadFloat1(const ReadbackCapture &capture, std::uint32_t x, std::uint32_t y,
                float *result)
{
    return result != nullptr && ReadPixelBytes(capture, x, y, result, sizeof(*result));
}

bool WaitForQueue(ID3D12Device *device, ID3D12CommandQueue *queue)
{
    ComPtr<ID3D12Fence> fence;
    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) return false;
    constexpr UINT64 value = 1;
    if (FAILED(queue->Signal(fence.Get(), value))) return false;
    if (fence->GetCompletedValue() >= value) return true;

    const HANDLE eventHandle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (eventHandle == nullptr) return false;
    const HRESULT setResult = fence->SetEventOnCompletion(value, eventHandle);
    const DWORD waitResult = SUCCEEDED(setResult) ? WaitForSingleObject(eventHandle, 10000) : WAIT_FAILED;
    CloseHandle(eventHandle);
    return SUCCEEDED(setResult) && waitResult == WAIT_OBJECT_0;
}

bool HasDebugErrors(ID3D12Device *device)
{
    ComPtr<ID3D12InfoQueue> infoQueue;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&infoQueue)))) return false;
    bool found = false;
    const UINT64 count = infoQueue->GetNumStoredMessagesAllowedByRetrievalFilter();
    for (UINT64 index = 0; index < count; ++index) {
        SIZE_T size = 0;
        if (FAILED(infoQueue->GetMessage(index, nullptr, &size)) || size == 0) continue;
        std::vector<std::byte> storage(size);
        auto *message = reinterpret_cast<D3D12_MESSAGE *>(storage.data());
        if (FAILED(infoQueue->GetMessage(index, message, &size))) continue;
        if (message->Severity == D3D12_MESSAGE_SEVERITY_ERROR ||
            message->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION) {
            std::cerr << "D3D12 validation: " << message->pDescription << '\n';
            found = true;
        }
    }
    return found;
}

// Native x of the left edge of the 1:1 centre band and of the raw Work region: the outline shader
// paints the two pixels just inside each edge, so ceil(edge) lands on the band.
std::uint32_t CenterLeft(const pw::LayoutV1 &layout)
{
    return static_cast<std::uint32_t>(std::ceil(
        0.5f * static_cast<float>(layout.nativeWidth) * (1.0f - layout.centerFractionX)));
}
std::uint32_t CenterLeft(const pw::LayoutV2 &layout)
{
    const float n = static_cast<float>(layout.nativeWidth);
    return static_cast<std::uint32_t>(std::ceil(
        0.5f * n + layout.centerOffsetX * n - 0.5f * layout.centerFractionX * n));
}
std::uint32_t WorkLeft(const pw::LayoutV1 &layout)
{
    return (layout.nativeWidth - layout.workWidth) / 2u;
}
std::uint32_t WorkLeft(const pw::LayoutV2 &layout)
{
    const float n = static_cast<float>(layout.nativeWidth);
    const float bandCenter = 0.5f * n + layout.centerOffsetX * n;
    const float periphery = bandCenter - 0.5f * layout.centerFractionX * n;
    return static_cast<std::uint32_t>(std::ceil(periphery - layout.compressionXNeg * periphery));
}

template <typename Layout>
bool ExecuteRoundTrip(ID3D12Device *device, ID3D12CommandQueue *queue,
                      pw::D3D12Adapter *adapter, const Layout &layout,
                      const pw::D3D12TargetFormats &formats, float expectGain = 1.0f, float expectGamma = 1.0f)
{
    constexpr std::array<FLOAT, 4> colorValue{0.25f, 0.5f, 0.75f, 1.0f};
    // What the unpacked colour should be after the adapter's output adjustment.
    const std::array<float, 4> expectedColor{
        expectGain * std::pow(colorValue[0], 1.0f / expectGamma), expectGain * std::pow(colorValue[1], 1.0f / expectGamma),
        expectGain * std::pow(colorValue[2], 1.0f / expectGamma), colorValue[3]};
    constexpr std::array<FLOAT, 4> depthValue{0.625f, 0.0f, 0.0f, 0.0f};
    constexpr std::array<FLOAT, 4> motionValue{12.0f, -6.0f, 0.0f, 0.0f};
    constexpr std::array<FLOAT, 4> confidenceValue{0.875f, 0.0f, 0.0f, 0.0f};
    const std::array<DXGI_FORMAT, 4> resourceFormats{
        formats.color, formats.depth, formats.motion, formats.confidence};

    std::array<ComPtr<ID3D12Resource>, 4> sources;
    std::array<ComPtr<ID3D12Resource>, 4> outputs;
    for (std::size_t index = 0; index < sources.size(); ++index) {
        sources[index] = CreateTexture(
            device, layout.nativeWidth, layout.nativeHeight, resourceFormats[index],
            D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        outputs[index] = CreateTexture(
            device, layout.nativeWidth, layout.nativeHeight, resourceFormats[index],
            D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        if (!sources[index] || !outputs[index]) return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{};
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.NumDescriptors = 8;
    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    if (FAILED(device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&rtvHeap)))) return false;
    const UINT rtvIncrement = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 8> rtvHandles{};
    rtvHandles[0] = rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (std::size_t index = 1; index < rtvHandles.size(); ++index) {
        rtvHandles[index] = rtvHandles[index - 1];
        rtvHandles[index].ptr += rtvIncrement;
    }
    for (std::size_t index = 0; index < sources.size(); ++index) {
        device->CreateRenderTargetView(sources[index].Get(), nullptr, rtvHandles[index]);
        device->CreateRenderTargetView(outputs[index].Get(), nullptr, rtvHandles[index + 4]);
    }

    const pw::D3D12SourceResources nativeSources{
        {sources[0].Get(), formats.color}, {sources[1].Get(), formats.depth},
        {sources[2].Get(), formats.motion}, {sources[3].Get(), formats.confidence}};
    if (adapter->WriteSourceDescriptors(0, nativeSources) != pw::AdapterStatus::Ok) return false;

    const pw::D3D12PackedViews packed = adapter->PackedViews(0);
    std::array<ReadbackCapture, 4> packedReadbacks;
    std::array<ReadbackCapture, 4> outputReadbacks;
    const std::array<ID3D12Resource *, 4> packedResources{
        packed.resources.color.resource, packed.resources.depth.resource,
        packed.resources.motion.resource, packed.resources.confidence.resource};
    if (adapter->WriteSourceDescriptors(1, packed.resources) != pw::AdapterStatus::Ok)
        return false;
    for (std::size_t index = 0; index < packedResources.size(); ++index) {
        packedReadbacks[index] = CreateReadback(device, packedResources[index]);
        outputReadbacks[index] = CreateReadback(device, outputs[index].Get());
        if (!packedReadbacks[index].buffer || !outputReadbacks[index].buffer) return false;
    }

    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
            IID_PPV_ARGS(&list))))
        return false;

    for (ID3D12Resource *source : {sources[0].Get(), sources[1].Get(),
                                  sources[2].Get(), sources[3].Get()})
        Transition(list.Get(), source, D3D12_RESOURCE_STATE_COMMON,
                   D3D12_RESOURCE_STATE_RENDER_TARGET);
    list->ClearRenderTargetView(rtvHandles[0], colorValue.data(), 0, nullptr);
    list->ClearRenderTargetView(rtvHandles[1], depthValue.data(), 0, nullptr);
    list->ClearRenderTargetView(rtvHandles[2], motionValue.data(), 0, nullptr);
    list->ClearRenderTargetView(rtvHandles[3], confidenceValue.data(), 0, nullptr);
    for (ID3D12Resource *source : {sources[0].Get(), sources[1].Get(),
                                  sources[2].Get(), sources[3].Get()})
        Transition(list.Get(), source, D3D12_RESOURCE_STATE_RENDER_TARGET,
                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    for (ID3D12Resource *resource : packedResources)
        Transition(list.Get(), resource, D3D12_RESOURCE_STATE_COMMON,
                   D3D12_RESOURCE_STATE_RENDER_TARGET);
    if (adapter->RecordPack(list.Get(), 0) != pw::AdapterStatus::Ok) return false;
    for (ID3D12Resource *resource : packedResources)
        Transition(list.Get(), resource, D3D12_RESOURCE_STATE_RENDER_TARGET,
                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    for (const ComPtr<ID3D12Resource> &output : outputs)
        Transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_COMMON,
                   D3D12_RESOURCE_STATE_RENDER_TARGET);
    const pw::D3D12TargetHandles nativeTargets{
        rtvHandles[4], rtvHandles[5], rtvHandles[6], rtvHandles[7]};
    if (adapter->RecordUnpackOwned(list.Get(), 0, nativeTargets) != pw::AdapterStatus::Ok)
        return false;
    const pw::DiagnosticOutlineFlags bothOutlines =
        pw::DiagnosticOutlineCenter | pw::DiagnosticOutlineRawWork;
    if (adapter->RecordUnpack(list.Get(), 1, nativeTargets, bothOutlines) !=
        pw::AdapterStatus::Ok)
        return false;

    for (ID3D12Resource *resource : packedResources)
        Transition(list.Get(), resource, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_COPY_SOURCE);
    for (const ComPtr<ID3D12Resource> &output : outputs)
        Transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                   D3D12_RESOURCE_STATE_COPY_SOURCE);
    for (std::size_t index = 0; index < packedResources.size(); ++index) {
        RecordReadback(list.Get(), packedResources[index], packedReadbacks[index]);
        RecordReadback(list.Get(), outputs[index].Get(), outputReadbacks[index]);
    }

    if (FAILED(list->Close())) return false;
    ID3D12CommandList *lists[] = {list.Get()};
    queue->ExecuteCommandLists(1, lists);
    if (!WaitForQueue(device, queue)) return false;

    const auto validateNativePixel = [&](std::uint32_t x, std::uint32_t y) {
        std::array<float, 4> color{};
        std::array<float, 2> motion{};
        float depth = 0.0f;
        float confidence = 0.0f;
        Check(ReadHalf4(outputReadbacks[0], x, y, &color), "native color readback succeeds");
        Check(ReadFloat1(outputReadbacks[1], x, y, &depth), "native depth readback succeeds");
        Check(ReadHalf2(outputReadbacks[2], x, y, &motion), "native motion readback succeeds");
        Check(ReadHalf1(outputReadbacks[3], x, y, &confidence), "native confidence readback succeeds");
        Check(Near(color[0], expectedColor[0]) && Near(color[1], expectedColor[1]) &&
                  Near(color[2], expectedColor[2]) && Near(color[3], expectedColor[3]),
              "constant color survives GPU Pack and Unpack (with the output adjustment applied)");
        Check(Near(depth, depthValue[0], 1.0e-6f), "point depth survives GPU Pack and Unpack");
        Check(Near(motion[0], motionValue[0], 0.15f) &&
                  Near(motion[1], motionValue[1], 0.15f),
              "endpoint motion survives GPU Pack and Unpack");
        Check(Near(confidence, confidenceValue[0]),
              "conservative confidence survives a constant GPU fixture");
    };
    validateNativePixel(layout.nativeWidth / 2u, layout.nativeHeight / 2u);
    validateNativePixel(12, layout.nativeHeight / 2u);

    std::array<float, 4> centerOutline{};
    std::array<float, 4> workOutline{};
    const std::uint32_t centerLeft = CenterLeft(layout);
    const std::uint32_t workLeft = WorkLeft(layout);
    Check(ReadHalf4(outputReadbacks[0], centerLeft, layout.nativeHeight / 2u,
                    &centerOutline) &&
              Near(centerOutline[0], 0.0f) && Near(centerOutline[1], 0.82f) &&
              Near(centerOutline[2], 1.0f),
          "integrated Unpack draws the cyan Center boundary at the exact pixel");
    Check(ReadHalf4(outputReadbacks[0], workLeft, layout.nativeHeight / 2u,
                    &workOutline) &&
              Near(workOutline[0], 1.0f) && Near(workOutline[1], 0.45f) &&
              Near(workOutline[2], 0.0f),
          "the same Unpack draw emits the orange raw Work boundary");

    const auto packedMotionAtNative = [&](pw::Float2 nativePixel,
                                          std::array<float, 2> *actual,
                                          pw::Float2 *expected) {
        const pw::Float2 mapped = pw::PackPosition(nativePixel, layout);
        const std::uint32_t x = static_cast<std::uint32_t>(std::floor(mapped.x));
        const std::uint32_t y = static_cast<std::uint32_t>(std::floor(mapped.y));
        const pw::Float2 guideCenter{static_cast<float>(x) + 0.5f,
                                     static_cast<float>(y) + 0.5f};
        const pw::Float2 nativeGuide = pw::UnpackPosition(guideCenter, layout);
        *expected = pw::PackMotion(nativeGuide, {motionValue[0], motionValue[1]}, layout);
        return ReadHalf2(packedReadbacks[2], x, y, actual);
    };

    std::array<float, 2> centerPacked{};
    std::array<float, 2> peripheralPacked{};
    pw::Float2 centerExpected{};
    pw::Float2 peripheralExpected{};
    Check(packedMotionAtNative(
              {static_cast<float>(layout.nativeWidth) * 0.5f + 0.5f,
               static_cast<float>(layout.nativeHeight) * 0.5f + 0.5f},
              &centerPacked, &centerExpected),
          "center packed-motion readback succeeds");
    Check(packedMotionAtNative(
              {12.5f, static_cast<float>(layout.nativeHeight) * 0.5f + 0.5f},
              &peripheralPacked, &peripheralExpected),
          "peripheral packed-motion readback succeeds");
    Check(Near(centerPacked[0], centerExpected.x, 0.03f) &&
              Near(centerPacked[1], centerExpected.y, 0.03f),
          "GPU center motion matches the CPU endpoint reference");
    Check(Near(peripheralPacked[0], peripheralExpected.x, 0.03f) &&
              Near(peripheralPacked[1], peripheralExpected.y, 0.03f),
          "GPU peripheral motion matches the nonlinear CPU endpoint reference");
    Check(std::abs(peripheralPacked[0] - centerPacked[0]) > 1.0f,
          "peripheral density changes packed motion rather than applying a global scale");
    return true;
}

} // namespace

int main(int argc, char **argv)
{
    if (argc != 4) {
        std::cerr << "Expected fullscreen, pack, and unpack DXBC paths\n";
        return EXIT_FAILURE;
    }
    const std::vector<char> vertex = ReadBinary(argv[1]);
    const std::vector<char> pack = ReadBinary(argv[2]);
    const std::vector<char> unpack = ReadBinary(argv[3]);
    Check(!vertex.empty() && !pack.empty() && !unpack.empty(),
          "compiled DXBC fixtures are readable");
    if (failures != 0) return EXIT_FAILURE;

    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();

    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter> warp;
    ComPtr<ID3D12Device> device;
    Check(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))), "DXGI factory is available");
    Check(factory && SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))),
          "WARP adapter is available");
    Check(warp && SUCCEEDED(D3D12CreateDevice(
                      warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))),
          "D3D12 WARP device is available");
    if (!device) return EXIT_FAILURE;

    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    Check(SUCCEEDED(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue))),
          "direct command queue is available");
    if (!queue) return EXIT_FAILURE;

    const pw::ShaderSet shaders{
        {vertex.data(), vertex.size()}, {pack.data(), pack.size()}, {unpack.data(), unpack.size()}};
    const pw::D3D12TargetFormats formats{
        DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R32_FLOAT,
        DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R16_FLOAT};
    pw::ConfigV1 config = pw::DefaultConfigV1();
    config.flags |= pw::ConfigFlagInputConfidenceValid;
    pw::LayoutV1 layout{};
    Check(pw::BuildLayout(config, 320, 180, &layout) == pw::Status::Ok,
          "smoke-test layout builds");
    Check(layout.workWidth == 288 && layout.workHeight == 162,
          "default nonlinear layout uses exact 90-percent work dimensions");

    pw::D3D12Adapter adapter;
    pw::LayoutV1 invalidLayout = layout;
    invalidLayout.edgeSlopeX = 0.75f;
    Check(adapter.Initialize(device.Get(), invalidLayout, formats, formats, shaders, 2) ==
              pw::AdapterStatus::InvalidArgument,
          "adapter rejects a noncanonical layout");
    Check(adapter.Initialize(device.Get(), layout, formats, formats, shaders, 2) ==
              pw::AdapterStatus::Ok,
          "adapter initializes on D3D12 WARP");

    const pw::D3D12PackedViews packed0 = adapter.PackedViews(0);
    const pw::D3D12PackedViews packed1 = adapter.PackedViews(1);
    Check(adapter.SourceDescriptorHeap() != nullptr, "adapter owns a shader-visible SRV heap");
    Check(packed0.resources.color.resource != nullptr && packed0.resources.depth.resource != nullptr &&
              packed0.resources.motion.resource != nullptr && packed0.resources.confidence.resource != nullptr,
          "adapter owns all four packed resources");
    Check(packed0.colorSrv.ptr != 0 && packed0.depthSrv.ptr != 0 && packed0.motionSrv.ptr != 0 &&
              packed0.confidenceSrv.ptr != 0 && packed0.shaderResourceTable.ptr != 0,
          "adapter exposes packed SRVs and their GPU table");
    Check(packed0.resources.color.resource != packed1.resources.color.resource,
          "frames in flight receive distinct packed resources");
    if (packed0.resources.color.resource != nullptr) {
        const D3D12_RESOURCE_DESC desc = packed0.resources.color.resource->GetDesc();
        Check(desc.Width == layout.workWidth && desc.Height == layout.workHeight &&
                  desc.Format == formats.color,
              "owned color resource matches work dimensions and target format");
    }
    Check(adapter.PackedViews(2).resources.color.resource == nullptr,
          "out-of-range packed view lookup is empty");

    pw::ConfigV2 scaledConfig = pw::DefaultConfigV2();
    scaledConfig.globalScalePercent = 50.0f;
    pw::LayoutV2 scaledLayout{};
    Check(pw::BuildLayout(scaledConfig, 320, 180, &scaledLayout) == pw::Status::Ok,
          "scaled adapter layout builds");
    pw::D3D12Adapter scaledAdapter;
    Check(scaledAdapter.Initialize(device.Get(), scaledLayout, formats, formats,
                                   shaders, 1) == pw::AdapterStatus::Ok,
          "adapter initializes directly from LayoutV2");
    const pw::D3D12PackedViews scaledPacked = scaledAdapter.PackedViews(0);
    Check(scaledPacked.resources.color.resource != nullptr &&
              scaledPacked.resources.color.resource->GetDesc().Width == scaledLayout.workWidth &&
              scaledPacked.resources.color.resource->GetDesc().Height == scaledLayout.workHeight,
          "LayoutV2 allocates packed resources at the actual NR extent");
    Check(scaledAdapter.Layout() == nullptr &&
              scaledAdapter.LayoutV2Description() != nullptr &&
              scaledAdapter.LayoutV2Description()->rawWorkWidth == scaledLayout.rawWorkWidth,
          "LayoutV2 initialization does not expose a misleading legacy layout");

    const auto color = CreateTexture(device.Get(), layout.nativeWidth, layout.nativeHeight, formats.color);
    const auto depth = CreateTexture(device.Get(), layout.nativeWidth, layout.nativeHeight, formats.depth);
    const auto motion = CreateTexture(device.Get(), layout.nativeWidth, layout.nativeHeight, formats.motion);
    const auto confidence = CreateTexture(
        device.Get(), layout.nativeWidth, layout.nativeHeight, formats.confidence);
    const auto wrongSizeMotion = CreateTexture(
        device.Get(), layout.workWidth, layout.workHeight, formats.motion);
    Check(color && depth && motion && confidence && wrongSizeMotion, "source fixtures allocate");
    const pw::D3D12SourceResources nativeSources{
        {color.Get(), formats.color}, {depth.Get(), formats.depth},
        {motion.Get(), formats.motion}, {confidence.Get(), formats.confidence}};
    Check(adapter.WriteSourceDescriptors(0, nativeSources) == pw::AdapterStatus::Ok,
          "native descriptors accept a complete canonical source set");

    pw::D3D12SourceResources mismatched = nativeSources;
    mismatched.motion.resource = wrongSizeMotion.Get();
    Check(adapter.WriteSourceDescriptors(0, mismatched) == pw::AdapterStatus::ResourceMismatch,
          "descriptor validation rejects mixed resource dimensions");
    Check(adapter.WriteSourceDescriptors(2, nativeSources) == pw::AdapterStatus::InvalidArgument,
          "descriptor validation rejects an out-of-range frame slot");

    pw::D3D12SourceResources missingConfidence = nativeSources;
    missingConfidence.confidence = {nullptr, DXGI_FORMAT_UNKNOWN};
    Check(adapter.WriteSourceDescriptors(0, missingConfidence) == pw::AdapterStatus::InvalidArgument,
          "confidence is required exactly when the layout flag is set");

    const auto depthTypeless = CreateTexture(
        device.Get(), 160, 90, DXGI_FORMAT_R16_TYPELESS);
    const auto motionRg32 = CreateTexture(
        device.Get(), 160, 90, DXGI_FORMAT_R32G32_FLOAT);
    Check(depthTypeless && motionRg32, "ABI-v2 guide fixtures allocate");
    pw::InputDescriptionV2 inputV2 = pw::DefaultInputDescriptionV2(
        layout.nativeWidth, layout.nativeHeight);
    inputV2.depthRect = {0, 0, 160, 90};
    inputV2.motionRect = {0, 0, 160, 90};
    inputV2.confidenceRect = {0, 0, 1, 1};
    inputV2.motionScaleX = 320.0f;
    inputV2.motionScaleY = 180.0f;
    inputV2.motionDirection = pw::MotionDirection::PreviousToCurrent;
    inputV2.depthConvention = pw::DepthConvention::Reversed;
    inputV2.colorEncoding = pw::ColorEncoding::LinearHdr;
    const pw::D3D12SourceResources universalSources{
        {color.Get(), formats.color}, {depthTypeless.Get(), DXGI_FORMAT_R16_UNORM},
        {motionRg32.Get(), DXGI_FORMAT_R32G32_FLOAT}, {nullptr, DXGI_FORMAT_UNKNOWN}};
    Check(adapter.WriteSourceDescriptorsV2(0, universalSources, inputV2) ==
              pw::AdapterStatus::Ok,
          "ABI-v2 accepts typeless+typed depth, RG32 motion, and half-resolution guides");
    inputV2.colorEncoding = pw::ColorEncoding::Unknown;
    Check(adapter.WriteSourceDescriptorsV2(0, universalSources, inputV2) ==
              pw::AdapterStatus::InvalidArgument,
          "ABI-v2 rejects an ambiguous colour space without writing targets");

    ComPtr<ID3D12InfoQueue> infoQueue;
    if (SUCCEEDED(device.As(&infoQueue))) infoQueue->ClearStoredMessages();
    Check(ExecuteRoundTrip(device.Get(), queue.Get(), &adapter, layout, formats),
          "WARP executes owned Pack, owned/external Unpack, copies, and readback");
    Check(device->GetDeviceRemovedReason() == S_OK,
          "D3D12 device remains healthy after the GPU round-trip");
    Check(!HasDebugErrors(device.Get()), "D3D12 debug layer reports no errors or corruption");

    // The same round-trip with the centre band moved off the frame centre (ABI v2 offset): the
    // per-side shader mapping must agree with the CPU reference, the outlines follow the band.
    pw::ConfigV2 offsetConfig = pw::DefaultConfigV2();
    offsetConfig.flags |= pw::ConfigFlagInputConfidenceValid;
    offsetConfig.centerOffsetXPercent = 6.0f;
    offsetConfig.centerOffsetYPercent = -9.5f;
    pw::LayoutV2 offsetLayout{};
    Check(pw::BuildLayout(offsetConfig, 320, 180, &offsetLayout) == pw::Status::Ok,
          "offset smoke-test layout builds");
    pw::D3D12Adapter offsetAdapter;
    Check(offsetAdapter.Initialize(device.Get(), offsetLayout, formats, formats, shaders, 2) ==
              pw::AdapterStatus::Ok,
          "adapter initializes with a centre offset");
    if (infoQueue) infoQueue->ClearStoredMessages();
    Check(ExecuteRoundTrip(device.Get(), queue.Get(), &offsetAdapter, offsetLayout, formats),
          "WARP executes the offset Pack/Unpack round-trip");
    Check(device->GetDeviceRemovedReason() == S_OK,
          "D3D12 device remains healthy after the offset round-trip");
    Check(!HasDebugErrors(device.Get()), "D3D12 debug layer reports no errors after the offset round-trip");

    // Output colour adjustment: the constant colour must come back scaled / gamma-mapped. Each
    // round-trip drives its own adapter (the round-trip leaves the owned textures in the states it
    // ends with, so it is not repeatable on one adapter).
    Check(offsetAdapter.SetOutputColorAdjust(0.0f, 1.0f) == pw::AdapterStatus::InvalidArgument,
          "a zero gain is rejected");
    pw::D3D12Adapter gainAdapter;
    Check(gainAdapter.Initialize(device.Get(), offsetLayout, formats, formats, shaders, 2) == pw::AdapterStatus::Ok &&
              gainAdapter.SetOutputColorAdjust(0.5f, 1.0f) == pw::AdapterStatus::Ok,
          "gain 0.5 is accepted");
    if (infoQueue) infoQueue->ClearStoredMessages();
    Check(ExecuteRoundTrip(device.Get(), queue.Get(), &gainAdapter, offsetLayout, formats, 0.5f, 1.0f),
          "WARP executes the round-trip with a colour gain");
    Check(device->GetDeviceRemovedReason() == S_OK && !HasDebugErrors(device.Get()),
          "D3D12 device and debug layer are clean after the gain round-trip");
    pw::D3D12Adapter gammaAdapter;
    Check(gammaAdapter.Initialize(device.Get(), offsetLayout, formats, formats, shaders, 2) == pw::AdapterStatus::Ok &&
              gammaAdapter.SetOutputColorAdjust(1.0f, 2.0f) == pw::AdapterStatus::Ok,
          "gamma 2 is accepted");
    if (infoQueue) infoQueue->ClearStoredMessages();
    Check(ExecuteRoundTrip(device.Get(), queue.Get(), &gammaAdapter, offsetLayout, formats, 1.0f, 2.0f),
          "WARP executes the round-trip with a colour gamma");
    Check(device->GetDeviceRemovedReason() == S_OK && !HasDebugErrors(device.Get()),
          "D3D12 device and debug layer are clean after the gamma round-trip");

    // Source-set ring: external source sets addressed independently of the texture slots, so a host
    // that changes its input resources every frame can write a fresh set per frame.
    {
        const pw::InputDescriptionV2 defaultInput = pw::DefaultInputDescriptionV2(layout.nativeWidth, layout.nativeHeight);
        pw::D3D12Adapter ringAdapter;
        Check(ringAdapter.Initialize(device.Get(), layout, formats, formats, shaders, 2, 1) == pw::AdapterStatus::InvalidArgument,
              "fewer source sets than frame slots are rejected");
        Check(ringAdapter.Initialize(device.Get(), layout, formats, formats, shaders, 2, 8) == pw::AdapterStatus::Ok &&
                  ringAdapter.SourceSetCount() == 8,
              "an adapter with a ring of eight source sets initializes");
        Check(ringAdapter.WriteSourceSetV2(7, nativeSources, defaultInput) == pw::AdapterStatus::Ok,
              "the last ring set accepts the native sources");
        Check(ringAdapter.WriteSourceSetV2(8, nativeSources, defaultInput) == pw::AdapterStatus::InvalidArgument,
              "an out-of-range source set is rejected");
        Check(ringAdapter.WriteSourceDescriptorsV2(1, nativeSources, defaultInput) == pw::AdapterStatus::Ok &&
                  ringAdapter.WriteSourceDescriptorsV2(2, nativeSources, defaultInput) == pw::AdapterStatus::InvalidArgument,
              "slot-addressed writes still stop at the frame slot count");
        ComPtr<ID3D12CommandAllocator> ringAllocator;
        ComPtr<ID3D12GraphicsCommandList> ringList;
        Check(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&ringAllocator))) &&
                  SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, ringAllocator.Get(), nullptr, IID_PPV_ARGS(&ringList))),
              "a command list for the ring checks is created");
        if (ringList) {
            Check(ringAdapter.RecordPackFromSet(ringList.Get(), 1, 7) == pw::AdapterStatus::Ok,
                  "a pack from a ring set into a different texture slot records");
            Check(ringAdapter.RecordPackFromSet(ringList.Get(), 0, 3) == pw::AdapterStatus::ResourceMismatch,
                  "a pack from an unwritten source set is rejected");
            Check(ringAdapter.RecordPackFromSet(ringList.Get(), 2, 7) == pw::AdapterStatus::InvalidArgument,
                  "a pack into an out-of-range texture slot is rejected");
            Check(ringAdapter.RecordPack(ringList.Get(), 1) == pw::AdapterStatus::Ok,
                  "the slot-addressed pack still works on a ring adapter");
            ringList->Close();
        }
    }

    if (failures != 0) {
        std::cerr << failures << " D3D12 smoke-test failure(s)\n";
        return EXIT_FAILURE;
    }
    std::cout << "PeripheralWarp D3D12 GPU smoke test passed\n";
    return EXIT_SUCCESS;
}
