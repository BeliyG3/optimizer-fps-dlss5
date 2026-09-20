#include "temporal_resources.h"

#include <cstdio>

namespace pwtemporal {
namespace {

D3D12_STATIC_SAMPLER_DESC StaticSampler(UINT reg, D3D12_FILTER filter)
{
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

constexpr DXGI_FORMAT kResidualFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
constexpr DXGI_FORMAT kChainFormat = DXGI_FORMAT_R16G16_FLOAT;
constexpr DXGI_FORMAT kExpectFormat = DXGI_FORMAT_R32G32B32A32_FLOAT;
constexpr std::uint32_t kTableSize = kTableUsed + 2;

bool CreatePso(ID3D12Device *device, ID3D12RootSignature *rootSignature,
               const std::vector<char> &code, ID3D12PipelineState **out)
{
    if (code.empty()) return false;
    D3D12_COMPUTE_PIPELINE_STATE_DESC d{};
    d.pRootSignature = rootSignature;
    d.CS = {code.data(), code.size()};
    return SUCCEEDED(device->CreateComputePipelineState(&d, IID_PPV_ARGS(out)));
}

} // namespace

bool Resources::Create(ID3D12Device *dev, const pwngx::Shaders &shaders, std::uint32_t nativeWidth,
                       std::uint32_t nativeHeight, DXGI_FORMAT outFormat, DXGI_FORMAT outView, std::uint32_t motionWidth,
                       std::uint32_t motionHeight, DXGI_FORMAT depthFmt, std::uint32_t depthWidth,
                       std::uint32_t depthHeight, char *error, std::size_t errorSize)
{
    auto fail = [&](const char *what) {
        if (error && errorSize) std::snprintf(error, errorSize, "temporal: %s", what);
        return false;
    };
    device = dev;
    device->AddRef();
    nativeW = nativeWidth; nativeH = nativeHeight;
    motionW = motionWidth; motionH = motionHeight;
    depthW = depthWidth; depthH = depthHeight;
    outputFormat = outFormat; outputView = outView; depthFormat = depthFmt;
    for (const DXGI_FORMAT format : {outputView, kResidualFormat, kChainFormat, kExpectFormat, DXGI_FORMAT_R32_FLOAT}) {
        D3D12_FEATURE_DATA_FORMAT_SUPPORT support{format};
        if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support))) ||
            (support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) == 0)
            return fail("temporal target format does not support typed UAV stores");
    }
    if (!history.Create(device, nativeW, nativeH)) return fail("older-pass allocation failed");

    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    auto &range = ranges[0];
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = kTableUsed;
    range.BaseShaderRegister = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 2;
    ranges[1].OffsetInDescriptorsFromTableStart = kTableUsed;
    D3D12_ROOT_PARAMETER params[3]{};
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[2].Constants.ShaderRegister = 1;
    params[2].Constants.Num32BitValues = 8;
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0;
    params[0].Constants.Num32BitValues = kConstantDwords;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 2;
    params[1].DescriptorTable.pDescriptorRanges = ranges;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    const D3D12_STATIC_SAMPLER_DESC samplers[] = {StaticSampler(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR),
                                                  StaticSampler(1, D3D12_FILTER_MIN_MAG_MIP_POINT)};
    D3D12_ROOT_SIGNATURE_DESC rs{};
    rs.NumParameters = 3;
    rs.pParameters = params;
    rs.NumStaticSamplers = 2;
    rs.pStaticSamplers = samplers;
    rs.Flags = D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS | D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
               D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;
    ID3DBlob *blob = nullptr;
    if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, nullptr)) || blob == nullptr)
        return fail("root signature serialisation failed");
    const HRESULT hr = device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rootSignature));
    blob->Release();
    if (FAILED(hr)) return fail("root signature creation failed");

#define PW_TEMPORAL_PASS(name, member, reads, outputs, extent) \
    if (!CreatePso(device, rootSignature, shaders.temporal##name, &member##Pso)) \
        return fail("compute pipeline " #name " missing or unsupported");
#include "../../../shaders/temporal_passes.def"
#undef PW_TEMPORAL_PASS

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = kTableSlots * kTableSize;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&srvHeap)))) return fail("descriptor heap creation failed");
    srvIncrement = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    constexpr D3D12_RESOURCE_FLAGS kTarget = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (!pwngx::CreateTexture(device, nativeWidth, nativeHeight, kResidualFormat, kTarget, &residual) ||
        !pwngx::CreateTexture(device, nativeWidth, nativeHeight, kResidualFormat, kTarget, &residualPrev) ||
        !pwngx::CreateTexture(device, depthWidth, depthHeight, depthFormat, D3D12_RESOURCE_FLAG_NONE, &depthF) ||
        !pwngx::CreateTexture(device, motionWidth, motionHeight, kChainFormat, kTarget, &acc[0]) ||
        !pwngx::CreateTexture(device, motionWidth, motionHeight, kChainFormat, kTarget, &acc[1]) ||
        !pwngx::CreateTexture(device, motionWidth, motionHeight, kChainFormat, kTarget, &accP[0]) ||
        !pwngx::CreateTexture(device, motionWidth, motionHeight, kChainFormat, kTarget, &accP[1]) ||
        !pwngx::CreateTexture(device, nativeWidth, nativeHeight, outputView, kTarget, &interp))
        return fail("texture allocation failed");
    if (downsamplePso) {
        lowW = (nativeWidth + kLowDivisor - 1) / kLowDivisor;
        lowH = (nativeHeight + kLowDivisor - 1) / kLowDivisor;
        // 26.6.X: the reprojection's addition, smoothed by the compose pass.
        if (!pwngx::CreateTexture(device, lowW, lowH, kResidualFormat, kTarget, &residualLow) ||
            !pwngx::CreateTexture(device, nativeWidth, nativeHeight, kResidualFormat, kTarget, &toneAcc))
            return fail("low-res residual allocation failed");
    }
    // 26.28: the passes that only improve the result. Each is optional; without its texture the pass
    // is simply not recorded and the machine behaves as 26.27 did.
    if (expectPso && (!pwngx::CreateTexture(device, motionWidth, motionHeight, kExpectFormat, kTarget, &expect[0]) ||
                      !pwngx::CreateTexture(device, motionWidth, motionHeight, kExpectFormat, kTarget, &expect[1]) ||
                      !pwngx::CreateTexture(device, motionWidth, motionHeight, kExpectFormat, kTarget, &expectP[0]) ||
                      !pwngx::CreateTexture(device, motionWidth, motionHeight, kExpectFormat, kTarget, &expectP[1]) ||
                      !pwngx::CreateTexture(device, motionWidth, motionHeight, kExpectFormat, D3D12_RESOURCE_FLAG_NONE, &expectKick) ||
                      !pwngx::CreateTexture(device, depthWidth, depthHeight, depthFormat, D3D12_RESOURCE_FLAG_NONE, &depthPrev)))
        return fail("expected-depth allocation failed");
    if (residualOldPso && !pwngx::CreateTexture(device, nativeWidth, nativeHeight, kResidualFormat, kTarget, &residualOld))
        return fail("phase-in allocation failed");
    if (applyPso && !pwngx::CreateTexture(device, nativeWidth, nativeHeight, kResidualFormat, kTarget, &residualMix))
        return fail("residual-mix allocation failed");
    if (cellsPso && residualLow && (!pwngx::CreateTexture(device, lowW, lowH, kResidualFormat, kTarget, &cellsAdd) ||
                                    !pwngx::CreateTexture(device, lowW, lowH, kResidualFormat, kTarget, &cellsLook)))
        return fail("cell allocation failed");
    if (modelMotionPso && !pwngx::CreateTexture(device, motionWidth, motionHeight, kChainFormat, kTarget, &modelMv))
        return fail("model-motion allocation failed");

    const struct { ID3D12Resource *res; DXGI_FORMAT fmt; int target; } outputs[] = {
        {residual, kResidualFormat, kTargetResidualA},     {residualPrev, kResidualFormat, kTargetResidualB},
        {acc[0], kChainFormat, kTargetAcc0},               {acc[1], kChainFormat, kTargetAcc1},
        {accP[0], kChainFormat, kTargetAccPending0},       {accP[1], kChainFormat, kTargetAccPending1},
        {interp, outputView, kTargetInterp},               {residualLow, kResidualFormat, kTargetResidualLow},
        {toneAcc, kResidualFormat, kTargetAddition},       {residualOld, kResidualFormat, kTargetResidualOld},
        {expect[0], kExpectFormat, kTargetExpect0},        {expect[1], kExpectFormat, kTargetExpect1},
        {expectP[0], kExpectFormat, kTargetExpectPending0}, {expectP[1], kExpectFormat, kTargetExpectPending1},
        {cellsAdd, kResidualFormat, kTargetCellsAdd},      {cellsLook, kResidualFormat, kTargetCellsLook},
        {modelMv, kChainFormat, kTargetModelMv},           {residualMix, kResidualFormat, kTargetResidualMix}};
    for (const auto &r : outputs) {
        if (r.res == nullptr) continue;
        targets[r.target] = {r.res, r.fmt};
    }
    return true;
}

} // namespace pwtemporal
