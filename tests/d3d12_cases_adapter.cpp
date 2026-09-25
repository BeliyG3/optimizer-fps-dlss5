#include "d3d12_cases.h"

namespace d3d12_cases {

void RunAdapterCases(const std::vector<char> &vertex, const std::vector<char> &pack,
                     const std::vector<char> &unpack)
{
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
    if (!device) return;

    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    Check(SUCCEEDED(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue))),
          "direct command queue is available");
    if (!queue) return;

    const ofps::sdk::ShaderSet shaders{
        {vertex.data(), vertex.size()}, {pack.data(), pack.size()}, {unpack.data(), unpack.size()}};
    const ofps::sdk::D3D12TargetFormats formats{
        DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R32_FLOAT,
        DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R16_FLOAT};
    ofps::sdk::ConfigV1 config = ofps::sdk::DefaultConfigV1();
    config.flags |= ofps::sdk::ConfigFlagInputConfidenceValid;
    ofps::sdk::LayoutV1 layout{};
    Check(ofps::sdk::BuildLayout(config, 320, 180, &layout) == ofps::sdk::Status::Ok,
          "smoke-test layout builds");
    Check(layout.workWidth == 288 && layout.workHeight == 162,
          "default nonlinear layout uses exact 90-percent work dimensions");

    ofps::sdk::D3D12Adapter adapter;
    ofps::sdk::LayoutV1 invalidLayout = layout;
    invalidLayout.edgeSlopeX = 0.75f;
    Check(adapter.Initialize(device.Get(), invalidLayout, formats, formats, shaders, 2) ==
              ofps::sdk::AdapterStatus::InvalidArgument,
          "adapter rejects a noncanonical layout");
    Check(adapter.Initialize(device.Get(), layout, formats, formats, shaders, 2) ==
              ofps::sdk::AdapterStatus::Ok,
          "adapter initializes on D3D12 WARP");

    const ofps::sdk::D3D12PackedViews packed0 = adapter.PackedViews(0);
    const ofps::sdk::D3D12PackedViews packed1 = adapter.PackedViews(1);
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

    ofps::sdk::ConfigV2 scaledConfig = ofps::sdk::DefaultConfigV2();
    scaledConfig.globalScalePercent = 50.0f;
    ofps::sdk::LayoutV2 scaledLayout{};
    Check(ofps::sdk::BuildLayout(scaledConfig, 320, 180, &scaledLayout) == ofps::sdk::Status::Ok,
          "scaled adapter layout builds");
    ofps::sdk::D3D12Adapter scaledAdapter;
    Check(scaledAdapter.Initialize(device.Get(), scaledLayout, formats, formats,
                                   shaders, 1) == ofps::sdk::AdapterStatus::Ok,
          "adapter initializes directly from LayoutV2");
    const ofps::sdk::D3D12PackedViews scaledPacked = scaledAdapter.PackedViews(0);
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
    const ofps::sdk::D3D12SourceResources nativeSources{
        {color.Get(), formats.color}, {depth.Get(), formats.depth},
        {motion.Get(), formats.motion}, {confidence.Get(), formats.confidence}};
    Check(adapter.WriteSourceDescriptors(0, nativeSources) == ofps::sdk::AdapterStatus::Ok,
          "native descriptors accept a complete canonical source set");

    ofps::sdk::D3D12SourceResources mismatched = nativeSources;
    mismatched.motion.resource = wrongSizeMotion.Get();
    Check(adapter.WriteSourceDescriptors(0, mismatched) == ofps::sdk::AdapterStatus::ResourceMismatch,
          "descriptor validation rejects mixed resource dimensions");
    Check(adapter.WriteSourceDescriptors(2, nativeSources) == ofps::sdk::AdapterStatus::InvalidArgument,
          "descriptor validation rejects an out-of-range frame slot");

    ofps::sdk::D3D12SourceResources missingConfidence = nativeSources;
    missingConfidence.confidence = {nullptr, DXGI_FORMAT_UNKNOWN};
    Check(adapter.WriteSourceDescriptors(0, missingConfidence) == ofps::sdk::AdapterStatus::InvalidArgument,
          "confidence is required exactly when the layout flag is set");

    const auto depthTypeless = CreateTexture(
        device.Get(), 160, 90, DXGI_FORMAT_R16_TYPELESS);
    const auto motionRg32 = CreateTexture(
        device.Get(), 160, 90, DXGI_FORMAT_R32G32_FLOAT);
    Check(depthTypeless && motionRg32, "ABI-v2 guide fixtures allocate");
    ofps::sdk::InputDescriptionV2 inputV2 = ofps::sdk::DefaultInputDescriptionV2(
        layout.nativeWidth, layout.nativeHeight);
    inputV2.depthRect = {0, 0, 160, 90};
    inputV2.motionRect = {0, 0, 160, 90};
    inputV2.confidenceRect = {0, 0, 1, 1};
    inputV2.motionScaleX = 320.0f;
    inputV2.motionScaleY = 180.0f;
    inputV2.motionDirection = ofps::sdk::MotionDirection::PreviousToCurrent;
    inputV2.depthConvention = ofps::sdk::DepthConvention::Reversed;
    inputV2.colorEncoding = ofps::sdk::ColorEncoding::LinearHdr;
    const ofps::sdk::D3D12SourceResources universalSources{
        {color.Get(), formats.color}, {depthTypeless.Get(), DXGI_FORMAT_R16_UNORM},
        {motionRg32.Get(), DXGI_FORMAT_R32G32_FLOAT}, {nullptr, DXGI_FORMAT_UNKNOWN}};
    Check(adapter.WriteSourceDescriptorsV2(0, universalSources, inputV2) ==
              ofps::sdk::AdapterStatus::Ok,
          "ABI-v2 accepts typeless+typed depth, RG32 motion, and half-resolution guides");
    inputV2.colorEncoding = ofps::sdk::ColorEncoding::Unknown;
    Check(adapter.WriteSourceDescriptorsV2(0, universalSources, inputV2) ==
              ofps::sdk::AdapterStatus::InvalidArgument,
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
    ofps::sdk::ConfigV2 offsetConfig = ofps::sdk::DefaultConfigV2();
    offsetConfig.flags |= ofps::sdk::ConfigFlagInputConfidenceValid;
    offsetConfig.centerOffsetXPercent = 6.0f;
    offsetConfig.centerOffsetYPercent = -9.5f;
    ofps::sdk::LayoutV2 offsetLayout{};
    Check(ofps::sdk::BuildLayout(offsetConfig, 320, 180, &offsetLayout) == ofps::sdk::Status::Ok,
          "offset smoke-test layout builds");
    ofps::sdk::D3D12Adapter offsetAdapter;
    Check(offsetAdapter.Initialize(device.Get(), offsetLayout, formats, formats, shaders, 2) ==
              ofps::sdk::AdapterStatus::Ok,
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
    Check(offsetAdapter.SetOutputColorAdjust(0.0f, 1.0f) == ofps::sdk::AdapterStatus::InvalidArgument,
          "a zero gain is rejected");
    ofps::sdk::D3D12Adapter gainAdapter;
    Check(gainAdapter.Initialize(device.Get(), offsetLayout, formats, formats, shaders, 2) == ofps::sdk::AdapterStatus::Ok &&
              gainAdapter.SetOutputColorAdjust(0.5f, 1.0f) == ofps::sdk::AdapterStatus::Ok,
          "gain 0.5 is accepted");
    if (infoQueue) infoQueue->ClearStoredMessages();
    Check(ExecuteRoundTrip(device.Get(), queue.Get(), &gainAdapter, offsetLayout, formats, 0.5f, 1.0f),
          "WARP executes the round-trip with a colour gain");
    Check(device->GetDeviceRemovedReason() == S_OK && !HasDebugErrors(device.Get()),
          "D3D12 device and debug layer are clean after the gain round-trip");
    ofps::sdk::D3D12Adapter gammaAdapter;
    Check(gammaAdapter.Initialize(device.Get(), offsetLayout, formats, formats, shaders, 2) == ofps::sdk::AdapterStatus::Ok &&
              gammaAdapter.SetOutputColorAdjust(1.0f, 2.0f) == ofps::sdk::AdapterStatus::Ok,
          "gamma 2 is accepted");
    if (infoQueue) infoQueue->ClearStoredMessages();
    Check(ExecuteRoundTrip(device.Get(), queue.Get(), &gammaAdapter, offsetLayout, formats, 1.0f, 2.0f),
          "WARP executes the round-trip with a colour gamma");
    Check(device->GetDeviceRemovedReason() == S_OK && !HasDebugErrors(device.Get()),
          "D3D12 device and debug layer are clean after the gamma round-trip");

    // Four-channel motion vectors (RGBA16F and RGBA32F, as Cyberpunk 2077 with Ray Reconstruction hands
    // them over): accepted as sources and packed from .xy alone - the round-trip checks pack the same
    // motion as the two-channel source with z and w set to non-zero values.
    for (const DXGI_FORMAT wideMotion : {DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT}) {
        ofps::sdk::D3D12Adapter wideAdapter;
        Check(wideAdapter.Initialize(device.Get(), offsetLayout, formats, formats, shaders, 2) == ofps::sdk::AdapterStatus::Ok,
              "an adapter for four-channel motion initializes");
        if (infoQueue) infoQueue->ClearStoredMessages();
        Check(ExecuteRoundTrip(device.Get(), queue.Get(), &wideAdapter, offsetLayout, formats, 1.0f, 1.0f, wideMotion),
              wideMotion == DXGI_FORMAT_R16G16B16A16_FLOAT ? "WARP packs RGBA16F motion from .xy"
                                                           : "WARP packs RGBA32F motion from .xy");
        Check(device->GetDeviceRemovedReason() == S_OK && !HasDebugErrors(device.Get()),
              "D3D12 device and debug layer are clean after the four-channel motion round-trip");
    }

    // Source-set ring: external source sets addressed independently of the texture slots, so a host
    // that changes its input resources every frame can write a fresh set per frame.
    {
        const ofps::sdk::InputDescriptionV2 defaultInput = ofps::sdk::DefaultInputDescriptionV2(layout.nativeWidth, layout.nativeHeight);
        ofps::sdk::D3D12Adapter ringAdapter;
        Check(ringAdapter.Initialize(device.Get(), layout, formats, formats, shaders, 2, 1) == ofps::sdk::AdapterStatus::InvalidArgument,
              "fewer source sets than frame slots are rejected");
        Check(ringAdapter.Initialize(device.Get(), layout, formats, formats, shaders, 2, 8) == ofps::sdk::AdapterStatus::Ok &&
                  ringAdapter.SourceSetCount() == 8,
              "an adapter with a ring of eight source sets initializes");
        Check(ringAdapter.WriteSourceSetV2(7, nativeSources, defaultInput) == ofps::sdk::AdapterStatus::Ok,
              "the last ring set accepts the native sources");
        Check(ringAdapter.WriteSourceSetV2(8, nativeSources, defaultInput) == ofps::sdk::AdapterStatus::InvalidArgument,
              "an out-of-range source set is rejected");
        Check(ringAdapter.WriteSourceDescriptorsV2(1, nativeSources, defaultInput) == ofps::sdk::AdapterStatus::Ok &&
                  ringAdapter.WriteSourceDescriptorsV2(2, nativeSources, defaultInput) == ofps::sdk::AdapterStatus::InvalidArgument,
              "slot-addressed writes still stop at the frame slot count");
        ComPtr<ID3D12CommandAllocator> ringAllocator;
        ComPtr<ID3D12GraphicsCommandList> ringList;
        Check(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&ringAllocator))) &&
                  SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, ringAllocator.Get(), nullptr, IID_PPV_ARGS(&ringList))),
              "a command list for the ring checks is created");
        if (ringList) {
            Check(ringAdapter.RecordPackFromSet(ringList.Get(), 1, 7) == ofps::sdk::AdapterStatus::Ok,
                  "a pack from a ring set into a different texture slot records");
            Check(ringAdapter.RecordPackFromSet(ringList.Get(), 0, 3) == ofps::sdk::AdapterStatus::ResourceMismatch,
                  "a pack from an unwritten source set is rejected");
            Check(ringAdapter.RecordPackFromSet(ringList.Get(), 2, 7) == ofps::sdk::AdapterStatus::InvalidArgument,
                  "a pack into an out-of-range texture slot is rejected");
            Check(ringAdapter.RecordPack(ringList.Get(), 1) == ofps::sdk::AdapterStatus::Ok,
                  "the slot-addressed pack still works on a ring adapter");
            ringList->Close();
        }
    }
}

} // namespace d3d12_cases
