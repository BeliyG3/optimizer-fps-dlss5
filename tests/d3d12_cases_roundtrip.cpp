#include "d3d12_cases.h"

namespace d3d12_cases {
namespace {

bool Near(float actual, float expected, float tolerance = 0.02f)
{
    return std::abs(actual - expected) <= tolerance;
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

std::uint32_t CenterLeft(const ofps::sdk::LayoutV1 &layout)
{
    return static_cast<std::uint32_t>(std::ceil(
        0.5f * static_cast<float>(layout.nativeWidth) * (1.0f - layout.centerFractionX)));
}
std::uint32_t CenterLeft(const ofps::sdk::LayoutV2 &layout)
{
    const float n = static_cast<float>(layout.nativeWidth);
    return static_cast<std::uint32_t>(std::ceil(
        0.5f * n + layout.centerOffsetX * n - 0.5f * layout.centerFractionX * n));
}
std::uint32_t WorkLeft(const ofps::sdk::LayoutV1 &layout)
{
    return (layout.nativeWidth - layout.workWidth) / 2u;
}
std::uint32_t WorkLeft(const ofps::sdk::LayoutV2 &layout)
{
    const float n = static_cast<float>(layout.nativeWidth);
    const float bandCenter = 0.5f * n + layout.centerOffsetX * n;
    const float periphery = bandCenter - 0.5f * layout.centerFractionX * n;
    return static_cast<std::uint32_t>(std::ceil(periphery - layout.compressionXNeg * periphery));
}

template <typename Layout>
bool ExecuteRoundTrip(ID3D12Device *device, ID3D12CommandQueue *queue,
                      ofps::sdk::D3D12Adapter *adapter, const Layout &layout,
                      const ofps::sdk::D3D12TargetFormats &formats, float expectGain = 1.0f, float expectGamma = 1.0f,
                      DXGI_FORMAT sourceMotionFormat = DXGI_FORMAT_UNKNOWN)
{
    // The host's motion texture may be wider than the packed one (four-channel vectors, read as .xy).
    const DXGI_FORMAT motionSourceFormat = sourceMotionFormat != DXGI_FORMAT_UNKNOWN ? sourceMotionFormat : formats.motion;
    constexpr std::array<FLOAT, 4> colorValue{0.25f, 0.5f, 0.75f, 1.0f};
    // What the unpacked colour should be after the adapter's output adjustment.
    const std::array<float, 4> expectedColor{
        expectGain * std::pow(colorValue[0], 1.0f / expectGamma), expectGain * std::pow(colorValue[1], 1.0f / expectGamma),
        expectGain * std::pow(colorValue[2], 1.0f / expectGamma), colorValue[3]};
    constexpr std::array<FLOAT, 4> depthValue{0.625f, 0.0f, 0.0f, 0.0f};
    // z and w are not zero: a four-channel source must still pack exactly x and y.
    constexpr std::array<FLOAT, 4> motionValue{12.0f, -6.0f, 7.0f, 9.0f};
    constexpr std::array<FLOAT, 4> confidenceValue{0.875f, 0.0f, 0.0f, 0.0f};
    const std::array<DXGI_FORMAT, 4> resourceFormats{
        formats.color, formats.depth, motionSourceFormat, formats.confidence};
    const std::array<DXGI_FORMAT, 4> outputFormats{
        formats.color, formats.depth, formats.motion, formats.confidence};

    std::array<ComPtr<ID3D12Resource>, 4> sources;
    std::array<ComPtr<ID3D12Resource>, 4> outputs;
    for (std::size_t index = 0; index < sources.size(); ++index) {
        sources[index] = CreateTexture(
            device, layout.nativeWidth, layout.nativeHeight, resourceFormats[index],
            D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        outputs[index] = CreateTexture(
            device, layout.nativeWidth, layout.nativeHeight, outputFormats[index],
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

    const ofps::sdk::D3D12SourceResources nativeSources{
        {sources[0].Get(), formats.color}, {sources[1].Get(), formats.depth},
        {sources[2].Get(), motionSourceFormat}, {sources[3].Get(), formats.confidence}};
    if (adapter->WriteSourceDescriptors(0, nativeSources) != ofps::sdk::AdapterStatus::Ok) return false;

    const ofps::sdk::D3D12PackedViews packed = adapter->PackedViews(0);
    std::array<ReadbackCapture, 4> packedReadbacks;
    std::array<ReadbackCapture, 4> outputReadbacks;
    const std::array<ID3D12Resource *, 4> packedResources{
        packed.resources.color.resource, packed.resources.depth.resource,
        packed.resources.motion.resource, packed.resources.confidence.resource};
    if (adapter->WriteSourceDescriptors(1, packed.resources) != ofps::sdk::AdapterStatus::Ok)
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
    if (adapter->RecordPack(list.Get(), 0) != ofps::sdk::AdapterStatus::Ok) return false;
    for (ID3D12Resource *resource : packedResources)
        Transition(list.Get(), resource, D3D12_RESOURCE_STATE_RENDER_TARGET,
                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    for (const ComPtr<ID3D12Resource> &output : outputs)
        Transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_COMMON,
                   D3D12_RESOURCE_STATE_RENDER_TARGET);
    const ofps::sdk::D3D12TargetHandles nativeTargets{
        rtvHandles[4], rtvHandles[5], rtvHandles[6], rtvHandles[7]};
    if (adapter->RecordUnpackOwned(list.Get(), 0, nativeTargets) != ofps::sdk::AdapterStatus::Ok)
        return false;
    const ofps::sdk::DiagnosticOutlineFlags bothOutlines =
        ofps::sdk::DiagnosticOutlineCenter | ofps::sdk::DiagnosticOutlineRawWork;
    if (adapter->RecordUnpack(list.Get(), 1, nativeTargets, bothOutlines) !=
        ofps::sdk::AdapterStatus::Ok)
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

    const auto packedMotionAtNative = [&](ofps::sdk::Float2 nativePixel,
                                          std::array<float, 2> *actual,
                                          ofps::sdk::Float2 *expected) {
        const ofps::sdk::Float2 mapped = ofps::sdk::PackPosition(nativePixel, layout);
        const std::uint32_t x = static_cast<std::uint32_t>(std::floor(mapped.x));
        const std::uint32_t y = static_cast<std::uint32_t>(std::floor(mapped.y));
        const ofps::sdk::Float2 guideCenter{static_cast<float>(x) + 0.5f,
                                     static_cast<float>(y) + 0.5f};
        const ofps::sdk::Float2 nativeGuide = ofps::sdk::UnpackPosition(guideCenter, layout);
        *expected = ofps::sdk::PackMotion(nativeGuide, {motionValue[0], motionValue[1]}, layout);
        return ReadHalf2(packedReadbacks[2], x, y, actual);
    };

    std::array<float, 2> centerPacked{};
    std::array<float, 2> peripheralPacked{};
    ofps::sdk::Float2 centerExpected{};
    ofps::sdk::Float2 peripheralExpected{};
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

bool ExecuteRoundTrip(ID3D12Device *device, ID3D12CommandQueue *queue,
                      ofps::sdk::D3D12Adapter *adapter, const ofps::sdk::LayoutV1 &layout,
                      const ofps::sdk::D3D12TargetFormats &formats, float gain,
                      float gamma, DXGI_FORMAT motionFormat)
{
    return ExecuteRoundTrip<ofps::sdk::LayoutV1>(device, queue, adapter, layout,
                                                      formats, gain, gamma, motionFormat);
}

bool ExecuteRoundTrip(ID3D12Device *device, ID3D12CommandQueue *queue,
                      ofps::sdk::D3D12Adapter *adapter, const ofps::sdk::LayoutV2 &layout,
                      const ofps::sdk::D3D12TargetFormats &formats, float gain,
                      float gamma, DXGI_FORMAT motionFormat)
{
    return ExecuteRoundTrip<ofps::sdk::LayoutV2>(device, queue, adapter, layout,
                                                      formats, gain, gamma, motionFormat);
}

} // namespace d3d12_cases
