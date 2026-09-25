#include "core/warp/format_support.h"

#include "core/warp/compute.h"

namespace ofps::core::warp::detail {

std::string FormatName(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R32_FLOAT: return "R32_FLOAT";
    case DXGI_FORMAT_R16G16_FLOAT: return "R16G16_FLOAT";
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return "R16G16B16A16_FLOAT";
    case DXGI_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
    case DXGI_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
    case DXGI_FORMAT_R10G10B10A2_UNORM: return "R10G10B10A2_UNORM";
    case DXGI_FORMAT_R16G16B16A16_UNORM: return "R16G16B16A16_UNORM";
    default: return "DXGI_FORMAT(" + std::to_string(static_cast<unsigned>(format)) + ")";
    }
}

bool TypedStore(ID3D12Device* device, DXGI_FORMAT format) {
    if (!device || format == DXGI_FORMAT_UNKNOWN) return false;
    D3D12_FEATURE_DATA_FORMAT_SUPPORT data{format, D3D12_FORMAT_SUPPORT1_NONE,
                                           D3D12_FORMAT_SUPPORT2_NONE};
    return SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &data,
                                                  sizeof(data))) &&
           (data.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) != 0;
}

bool ShaderReadable(ID3D12Device* device, DXGI_FORMAT format) {
    if (!device || format == DXGI_FORMAT_UNKNOWN) return false;
    D3D12_FEATURE_DATA_FORMAT_SUPPORT data{format, D3D12_FORMAT_SUPPORT1_NONE,
                                           D3D12_FORMAT_SUPPORT2_NONE};
    return SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &data,
                                                  sizeof(data))) &&
           (data.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE) != 0;
}

bool ViewCompatible(DXGI_FORMAT resource, DXGI_FORMAT view) {
    if (resource == view) return true;
    switch (view) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return resource == DXGI_FORMAT_R8G8B8A8_TYPELESS;
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return resource == DXGI_FORMAT_B8G8R8A8_TYPELESS;
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        return resource == DXGI_FORMAT_R16G16B16A16_TYPELESS;
    case DXGI_FORMAT_R10G10B10A2_UNORM:
        return resource == DXGI_FORMAT_R10G10B10A2_TYPELESS;
    default: return false;
    }
}

} // namespace ofps::core::warp::detail

namespace ofps::core::warp {

PackDecision ProbePack(ID3D12Device* device, RequestedPath request,
                       D3D12_COMMAND_LIST_TYPE listType, DXGI_FORMAT colorView,
                       bool privateCompute, bool shadersLoaded, std::string& reason) {
    PackSupport s{};
    s.colorTypedStore = detail::TypedStore(device, colorView);
    s.depthTypedStore = detail::TypedStore(device, DXGI_FORMAT_R32_FLOAT);
    s.motionTypedStore = detail::TypedStore(device, DXGI_FORMAT_R16G16_FLOAT);
    s.shadersLoaded = shadersLoaded;
    s.sourcesValid = true; // per-frame resource/rect validation happens in Pack
    s.hostDirect = listType == D3D12_COMMAND_LIST_TYPE_DIRECT;
    s.privateCompute = privateCompute && listType == D3D12_COMMAND_LIST_TYPE_COMPUTE;
    const PackDecision d = DecidePack(request, s);
    if (d.reason == PathReason::MissingTypedColor ||
        d.reason == PathReason::MissingTypedDepth ||
        d.reason == PathReason::MissingTypedMotion) {
        const auto failed = d.reason == PathReason::MissingTypedColor ? colorView :
            d.reason == PathReason::MissingTypedDepth ? DXGI_FORMAT_R32_FLOAT :
            DXGI_FORMAT_R16G16_FLOAT;
        reason = "typed UAV store unsupported for " + detail::FormatName(failed);
    } else reason = ReasonText(d.reason);
    return d;
}

} // namespace ofps::core::warp
