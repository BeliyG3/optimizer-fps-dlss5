#include "hosts/reshade/menu_colour.h"

namespace ofps::reshade {

const char *MenuColourSpaceName(MenuColourSpace space) {
    switch (space) {
    case MenuColourSpace::Srgb: return "sRGB (G22 P709)";
    case MenuColourSpace::Scrgb: return "scRGB (G10 P709)";
    case MenuColourSpace::Hdr10Pq: return "HDR10 PQ (G2084 P2020)";
    case MenuColourSpace::Hdr10Hlg: return "HDR10 HLG";
    default: return "unknown";
    }
}

const char *MenuColourProblem(MenuColourSpace space, DXGI_FORMAT backBuffer) {
    switch (space) {
    case MenuColourSpace::Srgb: break;
    case MenuColourSpace::Scrgb: // as it is (owner ruling 2026-09-29): linear FP16 values go to NR unscaled
        return backBuffer == DXGI_FORMAT_R16G16B16A16_FLOAT ? nullptr : "scRGB swap chain without an FP16 back buffer";
    case MenuColourSpace::Hdr10Pq:
    case MenuColourSpace::Hdr10Hlg: return "HDR10 swap chain (not supported)";
    default: return "unknown swap chain colour space";
    }
    const bool sdr = backBuffer == DXGI_FORMAT_R8G8B8A8_UNORM || backBuffer == DXGI_FORMAT_B8G8R8A8_UNORM ||
                     backBuffer == DXGI_FORMAT_R10G10B10A2_UNORM;
    return sdr ? nullptr : "back buffer format not supported (sRGB-typed, typeless or float)";
}

DXGI_FORMAT MenuConvertViewFormat(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R11G11B10_FLOAT: return format;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}

bool MenuConvertBuild(ID3D12Device *device, const void *code, size_t size, MenuConvertPipeline *pipeline) {
    if (!device || !code || !size || !pipeline) return false;
    *pipeline = {};
    D3D12_DESCRIPTOR_RANGE ranges[2] = {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0}, {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 1}};
    D3D12_ROOT_PARAMETER rp{};
    rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp.DescriptorTable = {2, ranges};
    const D3D12_ROOT_SIGNATURE_DESC rd{1, &rp, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    Microsoft::WRL::ComPtr<ID3DBlob> blob, errors;
    if (FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors)) ||
        FAILED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&pipeline->root))))
        return false;
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = pipeline->root.Get();
    pd.CS = {code, size};
    return SUCCEEDED(device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pipeline->pso)));
}

void MenuConvertRecord(ID3D12GraphicsCommandList *list, const MenuConvertPipeline &pipeline, ID3D12DescriptorHeap *heap,
                       D3D12_GPU_DESCRIPTOR_HANDLE table, ID3D12Resource *target) {
    ID3D12DescriptorHeap *heaps[] = {heap};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(pipeline.root.Get());
    list->SetPipelineState(pipeline.pso.Get());
    list->SetComputeRootDescriptorTable(0, table);
    const D3D12_RESOURCE_DESC d = target->GetDesc();
    list->Dispatch((UINT(d.Width) + 7) / 8, (d.Height + 7) / 8, 1);
}

} // namespace ofps::reshade
