#include "bench_scene.h"

#include "bench_device.h"
#include "bench_options.h"
#include "bench_shaders.h"

#include <cstdio>

bool CreatePipeline(const BenchOptions &o, BenchDevice &d, BenchScene &s)
{
    // ---- shaders and state ----
    const char *source = o.flat ? kShaderFlat : kShader3D;
    {
        ComPtr<ID3DBlob> b;
        if (o.flat) {
            if (!(b = Compile(source, "VS", "vs_5_0"))) return false; d.dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &s.vsFlat);
            s.vsBlit = s.vsFlat;
        } else {
            if (!(b = Compile(source, "VSBox", "vs_5_0"))) return false; d.dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &s.vsBox);
            if (!(b = Compile(source, "VSGround", "vs_5_0"))) return false; d.dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &s.vsGround);
            if (!(b = Compile(source, "VSBlit", "vs_5_0"))) return false; d.dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &s.vsBlit);
            if (!(b = Compile(source, "VSBoxShadow", "vs_5_0"))) return false; d.dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &s.vsBoxShadow);
            if (!(b = Compile(source, "VSGroundShadow", "vs_5_0"))) return false; d.dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &s.vsGroundShadow);
            if (!(b = Compile(source, "VSMeshShadow", "vs_5_0"))) return false; d.dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &s.vsMeshShadow);
            if (!(b = Compile(source, "VSMesh", "vs_5_0"))) return false; d.dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &s.vsMesh);
            const D3D11_INPUT_ELEMENT_DESC layout[] = {
                {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
                {"PREVPOS", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
                {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0},
                {"COLOR", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 36, D3D11_INPUT_PER_VERTEX_DATA, 0},
                {"TEXCOORD", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 48, D3D11_INPUT_PER_VERTEX_DATA, 0},
                {"TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 0, 60, D3D11_INPUT_PER_VERTEX_DATA, 0}};
            if (FAILED(d.dev->CreateInputLayout(layout, 6, b->GetBufferPointer(), b->GetBufferSize(), &s.meshLayout))) { std::printf("[fail] mesh input layout\n"); return false; }
        }
        if (!(b = Compile(source, "PS", "ps_5_0"))) return false; d.dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &s.psScene);
        if (!(b = Compile(source, "PSBlit", "ps_5_0"))) return false; d.dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &s.psBlit);
        if (!o.flat) {
            if (!(b = Compile(source, "PSAccum", "ps_5_0"))) return false; d.dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &s.psAccum);
            if (!(b = Compile(source, "PSMeshShadow", "ps_5_0"))) return false; d.dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &s.psMeshShadow);
        }
    }
    D3D11_BUFFER_DESC bd{}; bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    bd.ByteWidth = o.flat ? 32 : sizeof(FrameConstants);
    d.dev->CreateBuffer(&bd, nullptr, &s.cb);
    bd.ByteWidth = sizeof(InstanceConstants);
    d.dev->CreateBuffer(&bd, nullptr, &s.cbInst);
    D3D11_DEPTH_STENCIL_DESC dsd{}; dsd.DepthEnable = GetEnvironmentVariableA("PW_BENCH_NO_DEPTH", nullptr, 0) ? FALSE : TRUE; dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL; // PW_BENCH_NO_DEPTH: debug
    dsd.DepthFunc = o.flat ? D3D11_COMPARISON_ALWAYS : (g_standardDepth ? D3D11_COMPARISON_LESS_EQUAL : D3D11_COMPARISON_GREATER_EQUAL); // reverse-Z unless --depth standard
    d.dev->CreateDepthStencilState(&dsd, &s.dss);
    D3D11_RASTERIZER_DESC rd{}; rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
    d.dev->CreateRasterizerState(&rd, &s.rs);
    D3D11_BLEND_DESC bld{}; bld.RenderTarget[0].BlendEnable = TRUE; bld.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE; bld.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
    bld.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD; bld.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE; bld.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    bld.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD; bld.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    bld.RenderTarget[1].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    d.dev->CreateBlendState(&bld, &s.additive);
    D3D11_SAMPLER_DESC smd{}; smd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR; smd.AddressU = smd.AddressV = smd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    d.dev->CreateSamplerState(&smd, &s.smp);

    // ---- directional shadow map (4096^2, normal 0..1 depth, comparison sampling) ----
    if (!o.flat) {
        D3D11_TEXTURE2D_DESC td{}; td.Width = td.Height = kShadowSize; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_R32_TYPELESS;
        td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(d.dev->CreateTexture2D(&td, nullptr, &s.shadowTex))) { std::printf("[fail] shadow map\n"); return false; }
        D3D11_DEPTH_STENCIL_VIEW_DESC sdv{}; sdv.Format = DXGI_FORMAT_D32_FLOAT; sdv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        d.dev->CreateDepthStencilView(s.shadowTex.Get(), &sdv, &s.shadowDsv);
        D3D11_SHADER_RESOURCE_VIEW_DESC ssv{}; ssv.Format = DXGI_FORMAT_R32_FLOAT; ssv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; ssv.Texture2D.MipLevels = 1;
        d.dev->CreateShaderResourceView(s.shadowTex.Get(), &ssv, &s.shadowSrv);
        D3D11_SAMPLER_DESC ssd{}; ssd.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
        ssd.AddressU = ssd.AddressV = ssd.AddressW = D3D11_TEXTURE_ADDRESS_BORDER; ssd.BorderColor[0] = ssd.BorderColor[1] = ssd.BorderColor[2] = ssd.BorderColor[3] = 1.0f;
        ssd.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL; ssd.MaxLOD = D3D11_FLOAT32_MAX;
        d.dev->CreateSamplerState(&ssd, &s.shadowSmp);
        D3D11_DEPTH_STENCIL_DESC sd2{}; sd2.DepthEnable = TRUE; sd2.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL; sd2.DepthFunc = D3D11_COMPARISON_LESS;
        d.dev->CreateDepthStencilState(&sd2, &s.dssShadow);
        if (!s.shadowDsv || !s.shadowSrv || !s.shadowSmp || !s.dssShadow) { std::printf("[fail] shadow map views\n"); return false; }
    }
    return true;
}
