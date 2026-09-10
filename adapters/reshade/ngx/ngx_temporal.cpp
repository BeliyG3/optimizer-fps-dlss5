#include "ngx_temporal.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace pwtemporal {
namespace {

using pwngx::Barrier;
using pwngx::BarrierExternal;

// Descriptor tables per ring: a fresh table per draw, at most 7 draws per interpolated frame over a
// queue at most 8 frames deep - 128 leaves an order of magnitude of headroom before a slot is reused.
constexpr std::uint32_t kSlots = 128;
constexpr std::uint32_t kTableSize = 9;   // t0..t8 (the register list lives in shaders/temporal.hlsl)
constexpr int kTableUsed = 9;
constexpr std::uint32_t kLowDivisor = 16;  // low-res residual: native / 16 per axis
constexpr D3D12_RESOURCE_STATES kReadable = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
constexpr D3D12_RESOURCE_STATES kAccState = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE; // model (compute) and our pixel passes

// RTV heap layout. The residual and both accumulation chains ping-pong, so each gets a pair; the
// indices travel with the textures when the chains are swapped (PromotePending, RecordResidual).
enum Rtv : int {
    kRtvResidualA = 0,
    kRtvResidualB,
    kRtvAcc0,
    kRtvAcc1,
    kRtvAccPending0,
    kRtvAccPending1,
    kRtvInterp,
    kRtvResidualLow,
    kRtvAddition,      // the reprojection's second target (26.6.X), smoothed by the compose pass
    kRtvCount
};

// Mirrors cbuffer PwTemporalConstants in shaders/temporal.hlsl: 9 float4 of root constants, every
// lane of which some pass reads (smoothing[2..3] pad to the float4 boundary). The shader carries the
// per-lane description.
struct Constants {
    float native[4];     // native width, height, 1/width, 1/height
    float colorRect[4];  // host colour sub-rect x, y, w, h
    float motionRect[4]; // host motion sub-rect x, y, w, h
    float depthRect[4];  // host depth sub-rect x, y, w, h
    float motionTex[4];  // motion texture width, height, MVecScale X, MVecScale Y
    float params[4];     // per-pass switches: validate link, accPrev valid / Catmull-Rom, debug vis, depth threshold
    float tune[4];       // colour tolerance, motion sign, raw interpolation, residual blend weight
    float fill[4];       // hole fill on/off, low-res width, low-res height, depth-guided chain fetch radius
    float smoothing[4];  // acceptance tap scale, guided smoothing radius, unused, unused
};
static_assert(sizeof(Constants) == 36 * sizeof(float));
constexpr UINT kConstantDwords = 36;

struct SlotKey {
    ID3D12Resource *res[kTableUsed] = {};
    DXGI_FORMAT fmt[kTableUsed] = {};
    bool valid = false;
    bool operator==(const SlotKey &o) const
    {
        if (!valid || !o.valid) return false;
        for (int i = 0; i < kTableUsed; ++i) if (res[i] != o.res[i] || fmt[i] != o.fmt[i]) return false;
        return true;
    }
};

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
    s.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    return s;
}

void SetRect(float (&out)[4], const Rect &r)
{
    out[0] = static_cast<float>(r.x);
    out[1] = static_cast<float>(r.y);
    out[2] = static_cast<float>(r.w);
    out[3] = static_cast<float>(r.h);
}

} // namespace

struct Machine::Impl {
    ID3D12Device *device = nullptr;
    ID3D12RootSignature *rootSignature = nullptr;
    ID3D12PipelineState *residualPso = nullptr, *accumulatePso = nullptr, *reprojectPso = nullptr, *downsamplePso = nullptr, *composePso = nullptr;
    // 26.6.X: the reprojection's addition + acceptance (RGBA16F native), smoothed by the compose pass.
    ID3D12Resource *toneAcc = nullptr;
    D3D12_RESOURCE_STATES toneAccState = D3D12_RESOURCE_STATE_COMMON;
    ID3D12DescriptorHeap *srvHeap = nullptr, *rtvHeap = nullptr;
    UINT srvIncrement = 0, rtvIncrement = 0;
    ID3D12Resource *residual = nullptr, *depthF = nullptr, *acc[2] = {}, *interp = nullptr;
    ID3D12Resource *residualPrev = nullptr; // the previous full pass's residual (ping-pong with `residual`)
    D3D12_RESOURCE_STATES residualPrevState = D3D12_RESOURCE_STATE_COMMON;
    int residualRtv = kRtvResidualA, residualPrevRtv = kRtvResidualB;
    bool residualEverWritten = false;
    ID3D12Resource *accP[2] = {}; // pending chain (background mode)
    D3D12_RESOURCE_STATES accPState[2] = {D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON};
    int accPCurrent = 0;
    int accRtv[2] = {kRtvAcc0, kRtvAcc1}, accPRtv[2] = {kRtvAccPending0, kRtvAccPending1}; // the indices follow the textures when the chains are swapped
    ID3D12Resource *colorF = nullptr; // snapshot of the host colour of the residual's frame (created on demand)
    ID3D12Resource *residualLow = nullptr; // box-filtered residual (hole fill); null when the shader is missing
    D3D12_RESOURCE_STATES residualLowState = D3D12_RESOURCE_STATE_COMMON;
    std::uint32_t lowW = 0, lowH = 0;
    D3D12_RESOURCE_STATES colorFState = D3D12_RESOURCE_STATE_COMMON;
    DXGI_FORMAT colorFFormat = DXGI_FORMAT_UNKNOWN;
    std::uint32_t colorFW = 0, colorFH = 0;
    D3D12_RESOURCE_STATES residualState = D3D12_RESOURCE_STATE_COMMON, depthFState = D3D12_RESOURCE_STATE_COMMON,
                          accState[2] = {D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON},
                          interpState = D3D12_RESOURCE_STATE_COMMON;
    int accCurrent = 0; // acc[accCurrent] holds the latest accumulation
    std::uint32_t nativeW = 0, nativeH = 0, motionW = 0, motionH = 0, depthW = 0, depthH = 0;
    DXGI_FORMAT outputFormat = DXGI_FORMAT_UNKNOWN, outputView = DXGI_FORMAT_UNKNOWN, depthFormat = DXGI_FORMAT_UNKNOWN;
    SlotKey keys[kSlots];
    std::uint32_t nextSlot = 0;

    ~Impl()
    {
        for (ID3D12Resource *r : {residual, residualPrev, depthF, acc[0], acc[1], interp, colorF, residualLow, accP[0], accP[1], toneAcc}) if (r) r->Release();
        if (srvHeap) srvHeap->Release();
        if (rtvHeap) rtvHeap->Release();
        for (ID3D12PipelineState *p : {residualPso, accumulatePso, reprojectPso, downsamplePso, composePso}) if (p) p->Release();
        if (rootSignature) rootSignature->Release();
        if (device) device->Release();
    }

    D3D12_CPU_DESCRIPTOR_HANDLE Rtv(int index) const
    {
        D3D12_CPU_DESCRIPTOR_HANDLE h = rtvHeap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += static_cast<SIZE_T>(index) * rtvIncrement;
        return h;
    }

    bool CreatePso(const pwngx::Shaders &shaders, const std::vector<char> &pixel, DXGI_FORMAT rt, ID3D12PipelineState **out, DXGI_FORMAT rt1 = DXGI_FORMAT_UNKNOWN)
    {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
        d.pRootSignature = rootSignature;
        d.VS = {shaders.vertex.data(), shaders.vertex.size()};
        d.PS = {pixel.data(), pixel.size()};
        d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        d.SampleMask = UINT_MAX;
        d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        d.RasterizerState.DepthClipEnable = TRUE;
        d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        d.NumRenderTargets = rt == DXGI_FORMAT_UNKNOWN ? 0 : (rt1 != DXGI_FORMAT_UNKNOWN ? 2 : 1);
        d.RTVFormats[0] = rt;
        if (rt1 != DXGI_FORMAT_UNKNOWN) { d.RTVFormats[1] = rt1; d.BlendState.RenderTarget[1].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL; }
        d.SampleDesc.Count = 1;
        return SUCCEEDED(device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(out)));
    }

    // Finds or writes the descriptor table for this input set; tables are never rewritten while
    // their inputs are the same, so nothing the GPU may still read changes.
    // A fresh table for every draw from a ring deep enough that the GPU has long finished with the
    // entry being rewritten (a keyed cache was rewritten under in-flight frames once the host rotated
    // its input textures every frame: garbage/black frames).
    D3D12_GPU_DESCRIPTOR_HANDLE Table(const SlotKey &key)
    {
        const std::uint32_t slot = nextSlot;
        nextSlot = (nextSlot + 1) % kSlots;
        keys[slot] = key;
        {
            D3D12_CPU_DESCRIPTOR_HANDLE h = srvHeap->GetCPUDescriptorHandleForHeapStart();
            h.ptr += static_cast<SIZE_T>(slot) * kTableSize * srvIncrement;
            for (int i = 0; i < kTableUsed; ++i) {
                D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
                const bool set = key.res[i] != nullptr;
                desc.Format = set ? key.fmt[i] : DXGI_FORMAT_R16G16B16A16_FLOAT;
                desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                desc.Texture2D.MipLevels = 1;
                device->CreateShaderResourceView(set ? key.res[i] : residual, &desc, h); // unset entries: the residual, never null
                h.ptr += srvIncrement;
            }
        }
        D3D12_GPU_DESCRIPTOR_HANDLE g = srvHeap->GetGPUDescriptorHandleForHeapStart();
        g.ptr += static_cast<UINT64>(slot) * kTableSize * srvIncrement;
        return g;
    }

    void Draw(ID3D12GraphicsCommandList *cmd, ID3D12PipelineState *pso, D3D12_CPU_DESCRIPTOR_HANDLE rtv, std::uint32_t w,
              std::uint32_t h, D3D12_GPU_DESCRIPTOR_HANDLE table, const Constants &constants, const D3D12_CPU_DESCRIPTOR_HANDLE *rtv1 = nullptr)
    {
        ID3D12DescriptorHeap *heaps[] = {srvHeap};
        cmd->SetDescriptorHeaps(1, heaps);
        cmd->SetGraphicsRootSignature(rootSignature);
        cmd->SetPipelineState(pso);
        cmd->SetGraphicsRoot32BitConstants(0, kConstantDwords, &constants, 0);
        cmd->SetGraphicsRootDescriptorTable(1, table);
        const D3D12_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h), 0.0f, 1.0f};
        const D3D12_RECT scissor{0, 0, static_cast<LONG>(w), static_cast<LONG>(h)};
        cmd->RSSetViewports(1, &viewport);
        cmd->RSSetScissorRects(1, &scissor);
        cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        if (rtv1) { const D3D12_CPU_DESCRIPTOR_HANDLE both[2] = {rtv, *rtv1}; cmd->OMSetRenderTargets(2, both, FALSE, nullptr); }
        else cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        cmd->DrawInstanced(3, 1, 0, 0);
    }

    // The inputs as the passes see them: with a base colour, it stands in for the host colour.
    FrameInputs Resolve(const FrameInputs &raw) const
    {
        FrameInputs in = raw;
        if (raw.base) {
            in.color = raw.base;
            in.colorView = outputView;
            in.colorRect = Rect{0, 0, nativeW, nativeH};
            in.colorState = raw.baseState;
        } else {
            in.colorState = raw.hostInputState;
        }
        return in;
    }

    Constants BaseConstants(const FrameInputs &in) const
    {
        Constants c{};
        c.native[0] = static_cast<float>(nativeW);
        c.native[1] = static_cast<float>(nativeH);
        c.native[2] = 1.0f / static_cast<float>(nativeW);
        c.native[3] = 1.0f / static_cast<float>(nativeH);
        SetRect(c.colorRect, in.colorRect);
        SetRect(c.motionRect, in.motionRect);
        SetRect(c.depthRect, in.depthRect);
        c.motionTex[0] = static_cast<float>(motionW);
        c.motionTex[1] = static_cast<float>(motionH);
        c.motionTex[2] = in.mvScaleX;
        c.motionTex[3] = in.mvScaleY;
        // params[0..2] are per-pass and stay zero here: each record function sets what its pass reads.
        c.params[3] = in.depthThreshold > 0.0f ? in.depthThreshold : 1.0e9f;
        c.tune[0] = in.colorTolerance;
        c.tune[1] = in.motionSign == 0.0f ? 1.0f : in.motionSign;
        c.tune[2] = in.rawInterpolation ? 1.0f : 0.0f;
        c.tune[3] = 0.0f; // residual blend weight, set by RecordResidual
        c.fill[0] = (in.holeFill && residualLow != nullptr) ? 1.0f : 0.0f;
        c.fill[1] = static_cast<float>(lowW);
        c.fill[2] = static_cast<float>(lowH);
        c.fill[3] = in.mvSearchRadiusPx > 1.5f ? in.mvSearchRadiusPx : 1.0f; // PSReproject fetches the chain with depth guidance
        c.smoothing[0] = 0.0f; // tap radius scale of the acceptance average (0 = 1)
        c.smoothing[1] = in.smoothRadius;
        return c;
    }
};

Machine::Machine() = default;

Machine::~Machine()
{
    delete impl_;
}

bool Machine::Initialize(ID3D12Device *device, const pwngx::Shaders &shaders, std::uint32_t nativeWidth, std::uint32_t nativeHeight,
                         DXGI_FORMAT outputFormat, DXGI_FORMAT outputView, std::uint32_t motionWidth, std::uint32_t motionHeight,
                         DXGI_FORMAT depthFormat, std::uint32_t depthWidth, std::uint32_t depthHeight, char *error, std::size_t errorSize)
{
    delete impl_;
    impl_ = nullptr;
    device_ = nullptr;
    accValid_ = hasResidual_ = false;
    auto fail = [&](const char *what) {
        if (error && errorSize) std::snprintf(error, errorSize, "temporal: %s", what);
        delete impl_;
        impl_ = nullptr;
        return false;
    };
    if (!shaders.TemporalLoaded() || shaders.vertex.empty()) return fail("temporal shaders missing (optimizer-fps-dlss5\\temporal_*.dxbc)");
    impl_ = new Impl;
    Impl &m = *impl_;
    m.device = device;
    device->AddRef();
    m.nativeW = nativeWidth; m.nativeH = nativeHeight;
    m.motionW = motionWidth; m.motionH = motionHeight;
    m.depthW = depthWidth; m.depthH = depthHeight;
    m.outputFormat = outputFormat; m.outputView = outputView; m.depthFormat = depthFormat;

    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = kTableUsed;
    range.BaseShaderRegister = 0;
    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0;
    params[0].Constants.Num32BitValues = kConstantDwords;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &range;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    const D3D12_STATIC_SAMPLER_DESC samplers[] = {StaticSampler(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR),
                                                  StaticSampler(1, D3D12_FILTER_MIN_MAG_MIP_POINT)};
    D3D12_ROOT_SIGNATURE_DESC rs{};
    rs.NumParameters = 2;
    rs.pParameters = params;
    rs.NumStaticSamplers = 2;
    rs.pStaticSamplers = samplers;
    rs.Flags = D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS | D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
               D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;
    ID3DBlob *blob = nullptr;
    if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, nullptr)) || blob == nullptr)
        return fail("root signature serialisation failed");
    const HRESULT hr = device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&m.rootSignature));
    blob->Release();
    if (FAILED(hr)) return fail("root signature creation failed");
    if (!m.CreatePso(shaders, shaders.temporalResidual, DXGI_FORMAT_R16G16B16A16_FLOAT, &m.residualPso) ||
        !m.CreatePso(shaders, shaders.temporalAccumulate, DXGI_FORMAT_R16G16_FLOAT, &m.accumulatePso) ||
        !m.CreatePso(shaders, shaders.temporalReproject, outputView, &m.reprojectPso, DXGI_FORMAT_R16G16B16A16_FLOAT))
        return fail("pipeline creation failed");
    if (!shaders.temporalCompose.empty() && !m.CreatePso(shaders, shaders.temporalCompose, outputView, &m.composePso))
        return fail("compose pipeline creation failed");
    if (!shaders.temporalDownsample.empty() && !m.CreatePso(shaders, shaders.temporalDownsample, DXGI_FORMAT_R16G16B16A16_FLOAT, &m.downsamplePso))
        return fail("downsample pipeline creation failed");

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = kSlots * kTableSize;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m.srvHeap)))) return fail("descriptor heap creation failed");
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = kRtvCount;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m.rtvHeap)))) return fail("RTV heap creation failed");
    m.srvIncrement = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    m.rtvIncrement = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    if (!pwngx::CreateTexture(device, nativeWidth, nativeHeight, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, &m.residual) ||
        !pwngx::CreateTexture(device, nativeWidth, nativeHeight, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, &m.residualPrev) ||
        !pwngx::CreateTexture(device, depthWidth, depthHeight, depthFormat, D3D12_RESOURCE_FLAG_NONE, &m.depthF) ||
        !pwngx::CreateTexture(device, motionWidth, motionHeight, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, &m.acc[0]) ||
        !pwngx::CreateTexture(device, motionWidth, motionHeight, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, &m.acc[1]) ||
        !pwngx::CreateTexture(device, nativeWidth, nativeHeight, outputView, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, &m.interp))
        return fail("texture allocation failed");
    if (m.downsamplePso) {
        m.lowW = (nativeWidth + kLowDivisor - 1) / kLowDivisor;
        m.lowH = (nativeHeight + kLowDivisor - 1) / kLowDivisor;
        if (!pwngx::CreateTexture(device, m.lowW, m.lowH, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, &m.residualLow))
            return fail("low-res residual allocation failed");
        D3D12_RENDER_TARGET_VIEW_DESC rd{};
        rd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        rd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        device->CreateRenderTargetView(m.residualLow, &rd, m.Rtv(kRtvResidualLow));
        // 26.6.X: the reprojection's addition, smoothed by the compose pass.
        if (!pwngx::CreateTexture(device, nativeWidth, nativeHeight, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, &m.toneAcc))
            return fail("addition buffer allocation failed");
        device->CreateRenderTargetView(m.toneAcc, &rd, m.Rtv(kRtvAddition));
    }
    if (!pwngx::CreateTexture(device, motionWidth, motionHeight, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, &m.accP[0]) ||
        !pwngx::CreateTexture(device, motionWidth, motionHeight, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, &m.accP[1]))
        return fail("pending accumulation allocation failed");
    const struct { ID3D12Resource *res; DXGI_FORMAT fmt; int rtv; } rtvs[] = {
        {m.residual, DXGI_FORMAT_R16G16B16A16_FLOAT, kRtvResidualA}, {m.residualPrev, DXGI_FORMAT_R16G16B16A16_FLOAT, kRtvResidualB},
        {m.acc[0], DXGI_FORMAT_R16G16_FLOAT, kRtvAcc0}, {m.acc[1], DXGI_FORMAT_R16G16_FLOAT, kRtvAcc1},
        {m.accP[0], DXGI_FORMAT_R16G16_FLOAT, kRtvAccPending0}, {m.accP[1], DXGI_FORMAT_R16G16_FLOAT, kRtvAccPending1},
        {m.interp, outputView, kRtvInterp}};
    for (const auto &r : rtvs) {
        D3D12_RENDER_TARGET_VIEW_DESC rd{};
        rd.Format = r.fmt;
        rd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        device->CreateRenderTargetView(r.res, &rd, m.Rtv(r.rtv));
    }
    device_ = device;
    return true;
}

bool Machine::Matches(std::uint32_t nativeWidth, std::uint32_t nativeHeight, DXGI_FORMAT outputFormat, std::uint32_t motionWidth,
                      std::uint32_t motionHeight, DXGI_FORMAT depthFormat, std::uint32_t depthWidth, std::uint32_t depthHeight) const
{
    return impl_ != nullptr && impl_->nativeW == nativeWidth && impl_->nativeH == nativeHeight && impl_->outputFormat == outputFormat &&
           impl_->motionW == motionWidth && impl_->motionH == motionHeight && impl_->depthFormat == depthFormat &&
           impl_->depthW == depthWidth && impl_->depthH == depthHeight;
}

void Machine::Invalidate()
{
    accValid_ = false;
    pendingValid_ = false;
    hasResidual_ = false;
    if (impl_) impl_->residualEverWritten = false; // nothing from before the reset is blended in
}

void Machine::PromotePending()
{
    Impl &m = *impl_;
    std::swap(m.acc[0], m.accP[0]); std::swap(m.acc[1], m.accP[1]);
    std::swap(m.accState[0], m.accPState[0]); std::swap(m.accState[1], m.accPState[1]);
    std::swap(m.accCurrent, m.accPCurrent);
    std::swap(m.accRtv[0], m.accPRtv[0]); std::swap(m.accRtv[1], m.accPRtv[1]);
    accValid_ = pendingValid_;
    pendingValid_ = false;
    pendingMirror_ = accValid_;
}

void Machine::RecordAccumulatePending(ID3D12GraphicsCommandList *cmd, const FrameInputs &rawIn)
{
    Impl &m = *impl_;
    const FrameInputs in = m.Resolve(rawIn);
    const int prev = m.accPCurrent;
    const int next = 1 - prev;
    BarrierExternal(cmd, in.motion, in.hostInputState, kReadable);
    if (pendingValid_) Barrier(cmd, m.accP[prev], m.accPState[prev], kReadable);
    Barrier(cmd, m.accP[next], m.accPState[next], D3D12_RESOURCE_STATE_RENDER_TARGET);
    SlotKey key;
    key.valid = true;
    key.res[0] = in.color; key.fmt[0] = in.colorView;
    key.res[1] = m.residual; key.fmt[1] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    key.res[2] = m.residual; key.fmt[2] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    key.res[3] = in.motion; key.fmt[3] = in.motionView;
    key.res[4] = m.accP[prev]; key.fmt[4] = DXGI_FORMAT_R16G16_FLOAT;
    key.res[5] = in.depth; key.fmt[5] = in.depthView;
    key.res[6] = m.depthF; key.fmt[6] = in.depthView;
    key.res[7] = m.colorF ? m.colorF : in.color; key.fmt[7] = in.colorView;
    key.res[8] = m.residualLow ? m.residualLow : m.residual; key.fmt[8] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    Constants c = m.BaseConstants(in);
    c.params[1] = pendingValid_ ? 1.0f : 0.0f;
    m.Draw(cmd, m.accumulatePso, m.Rtv(m.accPRtv[next]), m.motionW, m.motionH, m.Table(key), c);
    Barrier(cmd, m.accP[next], m.accPState[next], kAccState);
    BarrierExternal(cmd, in.motion, kReadable, in.hostInputState);
    m.accPCurrent = next;
    pendingValid_ = true;
}

void Machine::RecordCopyAcc(ID3D12GraphicsCommandList *cmd, bool pending, ID3D12Resource *dst, D3D12_RESOURCE_STATES dstState)
{
    Impl &m = *impl_;
    ID3D12Resource *src = pending ? m.accP[m.accPCurrent] : m.acc[m.accCurrent];
    D3D12_RESOURCE_STATES &srcState = pending ? m.accPState[m.accPCurrent] : m.accState[m.accCurrent];
    Barrier(cmd, src, srcState, D3D12_RESOURCE_STATE_COPY_SOURCE);
    BarrierExternal(cmd, dst, dstState, D3D12_RESOURCE_STATE_COPY_DEST);
    cmd->CopyResource(dst, src);
    BarrierExternal(cmd, dst, D3D12_RESOURCE_STATE_COPY_DEST, dstState);
    Barrier(cmd, src, srcState, kAccState);
}

ID3D12Resource *Machine::Acc() const { return impl_ ? impl_->acc[impl_->accCurrent] : nullptr; }

ID3D12Resource *Machine::NextAcc() const { return impl_ ? impl_->acc[1 - impl_->accCurrent] : nullptr; }

void Machine::RecordResidual(ID3D12GraphicsCommandList *cmd, const FrameInputs &rawIn, ID3D12Resource *fresh, D3D12_RESOURCE_STATES freshState)
{
    Impl &m = *impl_;
    const FrameInputs in = m.Resolve(rawIn);
    // Cross-pass blend: the previous residual moved to this pass's frame along the chain (or the kick's
    // chain copy in `motion`), where the depth still matches.
    const bool blend = in.residualBlend > 0.0f && m.residualEverWritten && (in.blendFromMotion ? in.motion != nullptr : accValid_);
    // Ping-pong: what was the residual becomes the previous one (read), the other texture is written.
    std::swap(m.residual, m.residualPrev);
    std::swap(m.residualState, m.residualPrevState);
    std::swap(m.residualRtv, m.residualPrevRtv);
    BarrierExternal(cmd, in.color, in.colorState, kReadable);
    BarrierExternal(cmd, fresh, freshState, kReadable);
    if (blend && in.blendFromMotion) BarrierExternal(cmd, in.motion, in.hostInputState, kReadable);
    Barrier(cmd, m.residualPrev, m.residualPrevState, kReadable);
    if (blend) {
        BarrierExternal(cmd, in.depth, in.depthState, kReadable, in.depthSubresource);
        Barrier(cmd, m.depthF, m.depthFState, kReadable);
        if (!in.blendFromMotion) Barrier(cmd, m.acc[m.accCurrent], m.accState[m.accCurrent], kAccState);
    }
    Barrier(cmd, m.residual, m.residualState, D3D12_RESOURCE_STATE_RENDER_TARGET);
    SlotKey key;
    key.valid = true;
    key.res[0] = in.color; key.fmt[0] = in.colorView;
    key.res[1] = fresh; key.fmt[1] = m.outputView;
    key.res[2] = m.residualPrev; key.fmt[2] = DXGI_FORMAT_R16G16B16A16_FLOAT; // the previous residual (blend source)
    key.res[3] = in.motion; key.fmt[3] = in.motionView;
    key.res[4] = (blend && in.blendFromMotion) ? in.motion : m.acc[m.accCurrent]; key.fmt[4] = DXGI_FORMAT_R16G16_FLOAT;
    key.res[5] = in.depth; key.fmt[5] = in.depthView;
    key.res[6] = m.depthF; key.fmt[6] = in.depthView;
    // Colour snapshot of this frame (same format and size as the host's colour, created on demand).
    {
        const D3D12_RESOURCE_DESC cd = in.color->GetDesc();
        if (m.colorF == nullptr || m.colorFFormat != cd.Format || m.colorFW != (std::uint32_t) cd.Width || m.colorFH != cd.Height) {
            if (m.colorF) { m.colorF->Release(); m.colorF = nullptr; }
            if (pwngx::CreateTexture(m.device, (std::uint32_t) cd.Width, cd.Height, cd.Format, D3D12_RESOURCE_FLAG_NONE, &m.colorF)) {
                m.colorFFormat = cd.Format; m.colorFW = (std::uint32_t) cd.Width; m.colorFH = cd.Height;
                m.colorFState = D3D12_RESOURCE_STATE_COMMON;
            }
        }
    }
    key.res[7] = m.colorF ? m.colorF : in.color; key.fmt[7] = in.colorView;
    key.res[8] = m.residualLow ? m.residualLow : fresh; key.fmt[8] = m.residualLow ? DXGI_FORMAT_R16G16B16A16_FLOAT : m.outputView;
    if (blend && m.colorF) Barrier(cmd, m.colorF, m.colorFState, kReadable); // the previous pass's colour snapshot gates the blend
    Constants rc = m.BaseConstants(in);
    rc.tune[3] = blend ? std::clamp(in.residualBlend, 0.0f, 0.9f) : 0.0f;
    if (m.colorF == nullptr) rc.tune[0] = 0.0f; // no snapshot yet: no colour gate
    m.Draw(cmd, m.residualPso, m.Rtv(m.residualRtv), m.nativeW, m.nativeH, m.Table(key), rc);
    m.residualEverWritten = true;
    if (blend) BarrierExternal(cmd, in.depth, kReadable, in.depthState, in.depthSubresource);
    Barrier(cmd, m.residual, m.residualState, kReadable);
    if (m.residualLow) {
        // Box-filter the residual for the hole fill (t2 = residual, now readable; t8 must not alias the target).
        SlotKey low = key;
        low.res[2] = m.residual; low.fmt[2] = DXGI_FORMAT_R16G16B16A16_FLOAT;
        low.res[8] = fresh; low.fmt[8] = m.outputView;
        Barrier(cmd, m.residualLow, m.residualLowState, D3D12_RESOURCE_STATE_RENDER_TARGET);
        m.Draw(cmd, m.downsamplePso, m.Rtv(kRtvResidualLow), m.lowW, m.lowH, m.Table(low), m.BaseConstants(in));
        Barrier(cmd, m.residualLow, m.residualLowState, kReadable);
    }
    BarrierExternal(cmd, fresh, kReadable, freshState);
    if (m.colorF) {
        // Colour snapshot of this frame: what the reprojection and the next pass's blend test against.
        BarrierExternal(cmd, in.color, kReadable, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Barrier(cmd, m.colorF, m.colorFState, D3D12_RESOURCE_STATE_COPY_DEST);
        cmd->CopyResource(m.colorF, in.color);
        Barrier(cmd, m.colorF, m.colorFState, kReadable);
        BarrierExternal(cmd, in.color, D3D12_RESOURCE_STATE_COPY_SOURCE, kReadable);
    }
    // Depth snapshot of this frame.
    BarrierExternal(cmd, in.color, kReadable, in.colorState);
    BarrierExternal(cmd, in.depth, in.depthState, D3D12_RESOURCE_STATE_COPY_SOURCE, in.depthSubresource);
    Barrier(cmd, m.depthF, m.depthFState, D3D12_RESOURCE_STATE_COPY_DEST);
    if (in.depthSubresource != D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES) {
        // Planar depth-stencil: copy the depth plane only (CopyResource would need both planes in COPY_SOURCE).
        D3D12_TEXTURE_COPY_LOCATION dstP{}, srcP{};
        dstP.pResource = m.depthF; dstP.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dstP.SubresourceIndex = 0;
        srcP.pResource = in.depth; srcP.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; srcP.SubresourceIndex = 0;
        cmd->CopyTextureRegion(&dstP, 0, 0, 0, &srcP, nullptr);
    } else {
        cmd->CopyResource(m.depthF, in.depth);
    }
    Barrier(cmd, m.depthF, m.depthFState, kReadable);
    BarrierExternal(cmd, in.depth, D3D12_RESOURCE_STATE_COPY_SOURCE, in.depthState, in.depthSubresource);
    hasResidual_ = true;
    accValid_ = false; // the displacement restarts from this frame
}

void Machine::RecordAccumulate(ID3D12GraphicsCommandList *cmd, const FrameInputs &rawIn)
{
    Impl &m = *impl_;
    const FrameInputs in = m.Resolve(rawIn);
    const int prev = m.accCurrent;
    const int next = 1 - prev;
    BarrierExternal(cmd, in.motion, in.hostInputState, kReadable);
    if (accValid_) Barrier(cmd, m.acc[prev], m.accState[prev], kReadable);
    Barrier(cmd, m.acc[next], m.accState[next], D3D12_RESOURCE_STATE_RENDER_TARGET);
    SlotKey key;
    key.valid = true;
    key.res[0] = in.color; key.fmt[0] = in.colorView;
    key.res[1] = m.residual; key.fmt[1] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    key.res[2] = m.residual; key.fmt[2] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    key.res[3] = in.motion; key.fmt[3] = in.motionView;
    key.res[4] = m.acc[prev]; key.fmt[4] = DXGI_FORMAT_R16G16_FLOAT;
    key.res[5] = in.depth; key.fmt[5] = in.depthView;
    key.res[6] = m.depthF; key.fmt[6] = in.depthView;
    key.res[7] = m.colorF ? m.colorF : in.color; key.fmt[7] = in.colorView;
    key.res[8] = m.residualLow ? m.residualLow : m.residual; key.fmt[8] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    Constants c = m.BaseConstants(in);
    c.params[0] = 1.0f; // validate each link against the residual's frame (depth + colour); the pending chain has no stored frame to test against
    c.params[1] = accValid_ ? 1.0f : 0.0f;
    m.Draw(cmd, m.accumulatePso, m.Rtv(m.accRtv[next]), m.motionW, m.motionH, m.Table(key), c);
    Barrier(cmd, m.acc[next], m.accState[next], kAccState);
    BarrierExternal(cmd, in.motion, kReadable, in.hostInputState);
    m.accCurrent = next;
    accValid_ = true;
}

void Machine::RecordReproject(ID3D12GraphicsCommandList *cmd, const FrameInputs &rawIn, ID3D12Resource *hostOutput,
                              D3D12_RESOURCE_STATES hostOutputState, std::uint32_t dstX, std::uint32_t dstY)
{
    Impl &m = *impl_;
    const FrameInputs in = m.Resolve(rawIn);
    BarrierExternal(cmd, in.color, in.colorState, kReadable);
    BarrierExternal(cmd, in.depth, in.depthState, kReadable, in.depthSubresource);
    Barrier(cmd, m.acc[m.accCurrent], m.accState[m.accCurrent], kAccState);
    Barrier(cmd, m.residual, m.residualState, kReadable);
    Barrier(cmd, m.depthF, m.depthFState, kReadable);
    Barrier(cmd, m.interp, m.interpState, D3D12_RESOURCE_STATE_RENDER_TARGET);
    SlotKey key;
    key.valid = true;
    key.res[0] = in.color; key.fmt[0] = in.colorView;
    key.res[1] = m.residual; key.fmt[1] = DXGI_FORMAT_R16G16B16A16_FLOAT; // t1 must be bound to something (the reprojection does not read it)
    key.res[2] = m.residual; key.fmt[2] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    key.res[3] = in.motion; key.fmt[3] = in.motionView;
    key.res[4] = m.acc[m.accCurrent]; key.fmt[4] = DXGI_FORMAT_R16G16_FLOAT;
    key.res[5] = in.depth; key.fmt[5] = in.depthView;
    key.res[6] = m.depthF; key.fmt[6] = in.depthView;
    key.res[7] = m.colorF ? m.colorF : in.color; key.fmt[7] = in.colorView;
    key.res[8] = m.residualLow ? m.residualLow : m.residual; key.fmt[8] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    if (m.residualLow) Barrier(cmd, m.residualLow, m.residualLowState, kReadable);
    if (m.colorF) Barrier(cmd, m.colorF, m.colorFState, kReadable);
    Constants c = m.BaseConstants(in);
    if (m.colorF == nullptr) c.tune[0] = 0.0f; // no snapshot: no colour test
    c.params[1] = in.residualCatmullRom ? 1.0f : 0.0f; // the reprojection's residual filter (PSAccumulate uses params.y for the chain validity)
    c.params[2] = static_cast<float>(in.debugVis);
    // 26.6.X: the reprojection also writes its addition + acceptance; the compose pass then smooths the
    // addition along the original and writes the frame.
    const bool compose = m.composePso != nullptr && m.toneAcc != nullptr && in.smoothRadius > 0.0f && in.debugVis == 0 && !in.rawInterpolation;
    if (compose) {
        Barrier(cmd, m.toneAcc, m.toneAccState, D3D12_RESOURCE_STATE_RENDER_TARGET);
        const D3D12_CPU_DESCRIPTOR_HANDLE addRtv = m.Rtv(kRtvAddition);
        m.Draw(cmd, m.reprojectPso, m.Rtv(kRtvInterp), m.nativeW, m.nativeH, m.Table(key), c, &addRtv);
        Barrier(cmd, m.toneAcc, m.toneAccState, kReadable);
        SlotKey ck;
        ck.valid = true;
        ck.res[0] = in.color; ck.fmt[0] = in.colorView;
        ck.res[2] = m.toneAcc; ck.fmt[2] = DXGI_FORMAT_R16G16B16A16_FLOAT;
        ck.res[5] = in.depth; ck.fmt[5] = in.depthView;
        m.Draw(cmd, m.composePso, m.Rtv(kRtvInterp), m.nativeW, m.nativeH, m.Table(ck), c);
    } else {
        m.Draw(cmd, m.reprojectPso, m.Rtv(kRtvInterp), m.nativeW, m.nativeH, m.Table(key), c);
    }
    BarrierExternal(cmd, in.color, kReadable, in.colorState);
    BarrierExternal(cmd, in.depth, kReadable, in.depthState, in.depthSubresource);
    if (hostOutput == nullptr) return; // diagnostics: the target is kept, the host's Output is left alone
    Barrier(cmd, m.interp, m.interpState, D3D12_RESOURCE_STATE_COPY_SOURCE);
    BarrierExternal(cmd, hostOutput, hostOutputState, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = hostOutput;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = m.interp;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    const D3D12_BOX box{0, 0, 0, m.nativeW, m.nativeH, 1};
    cmd->CopyTextureRegion(&dst, dstX, dstY, 0, &src, &box);
    BarrierExternal(cmd, hostOutput, D3D12_RESOURCE_STATE_COPY_DEST, hostOutputState);
}

void Machine::RecordDebugCopies(ID3D12GraphicsCommandList *cmd, const FrameInputs &rawIn, ID3D12Resource *readback, std::uint32_t x,
                                std::uint32_t y)
{
    Impl &m = *impl_;
    const FrameInputs in = m.Resolve(rawIn);
    auto copyRow = [&](ID3D12Resource *src, DXGI_FORMAT format, std::uint32_t texelBytes, std::uint32_t sx, std::uint32_t sy, UINT64 offset) {
        const std::uint32_t texels = 256 / texelBytes;
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = readback;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Offset = offset;
        dst.PlacedFootprint.Footprint.Format = format;
        dst.PlacedFootprint.Footprint.Width = texels;
        dst.PlacedFootprint.Footprint.Height = 1;
        dst.PlacedFootprint.Footprint.Depth = 1;
        dst.PlacedFootprint.Footprint.RowPitch = 256;
        D3D12_TEXTURE_COPY_LOCATION s{};
        s.pResource = src;
        s.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        const D3D12_BOX box{sx, sy, 0, sx + texels, sy + 1, 1};
        cmd->CopyTextureRegion(&dst, 0, 0, 0, &s, &box);
    };
    BarrierExternal(cmd, in.color, in.colorState, D3D12_RESOURCE_STATE_COPY_SOURCE);
    copyRow(in.color, in.colorView, 8, in.colorRect.x + x, in.colorRect.y + y, 0);
    BarrierExternal(cmd, in.color, D3D12_RESOURCE_STATE_COPY_SOURCE, in.colorState);
    Barrier(cmd, m.residual, m.residualState, D3D12_RESOURCE_STATE_COPY_SOURCE);
    copyRow(m.residual, DXGI_FORMAT_R16G16B16A16_FLOAT, 8, x, y, 256);
    Barrier(cmd, m.residual, m.residualState, kReadable);
    Barrier(cmd, m.interp, m.interpState, D3D12_RESOURCE_STATE_COPY_SOURCE);
    copyRow(m.interp, m.outputView, 8, x, y, 512);
    Barrier(cmd, m.acc[m.accCurrent], m.accState[m.accCurrent], D3D12_RESOURCE_STATE_COPY_SOURCE);
    copyRow(m.acc[m.accCurrent], DXGI_FORMAT_R16G16_FLOAT, 4, in.motionRect.x + x * in.motionRect.w / m.nativeW,
            in.motionRect.y + y * in.motionRect.h / m.nativeH, 768);
    Barrier(cmd, m.acc[m.accCurrent], m.accState[m.accCurrent], kAccState);
}

} // namespace pwtemporal
