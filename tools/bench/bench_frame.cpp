#include "bench_render.h"

#include "bench_device.h"
#include "bench_options.h"
#include "bench_scene.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

// Jitter sequence (8 samples, the same for all frames of an 8-cycle).
void Jitter(int index, float *jx, float *jy)
{
    *jx = (float) (((index % 8) + 0.5) / 8.0 - 0.5);
    *jy = (float) ((((index * 3) % 8) + 0.5) / 8.0 - 0.5);
}

void RenderFlat(const BenchOptions &o, BenchDevice &d, BenchScene &s, int frame)
{
    const float vx = 0.30f, vy = 0.17f; // flat scene: uv per second
    const float time = frame / 60.0f;
    D3D11_MAPPED_SUBRESOURCE m{}; d.ctx->Map(s.cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m);
    float c[8] = {time, vx, vy, 1.0f / o.mvScale, o.mvDir, 0, 0, 0}; memcpy(m.pData, c, 32); d.ctx->Unmap(s.cb.Get(), 0);
    ID3D11RenderTargetView *rts[] = {d.colorRtv.Get(), d.motionRtv.Get()};
    d.ctx->OMSetRenderTargets(2, rts, d.dsv.Get());
    d.ctx->OMSetDepthStencilState(s.dss.Get(), 0);
    d.ctx->RSSetState(s.rs.Get());
    D3D11_VIEWPORT vp{0, 0, (float) kRenderW, (float) kRenderH, 0, 1}; d.ctx->RSSetViewports(1, &vp);
    d.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    d.ctx->VSSetShader(s.vsFlat.Get(), nullptr, 0); d.ctx->PSSetShader(s.psScene.Get(), nullptr, 0);
    d.ctx->PSSetConstantBuffers(0, 1, s.cb.GetAddressOf());
    d.ctx->Draw(3, 0);
    ID3D11RenderTargetView *none[2] = {}; d.ctx->OMSetRenderTargets(2, none, nullptr);
}

void RenderReference(const BenchOptions &o, BenchDevice &d, BenchScene &scene, GltfScene &gl, int frame)
{
    // Eight jittered renders averaged into the 4K colour target (additive blend, weight 1/8);
    // depth is cleared per sample so each sample resolves its own visibility.
    const float clear[4] = {0, 0, 0, 0};
    d.ctx->ClearRenderTargetView(d.outputRtv.Get(), clear);
    static const int refSamples = [] { char v[16] = {}; return GetEnvironmentVariableA("PW_BENCH_REF_SAMPLES", v, sizeof(v)) ? std::max(1, atoi(v)) : 8; }();
    for (int s = 0; s < refSamples; ++s) {
        float sx, sy; Jitter(s, &sx, &sy);
        d.ctx->ClearDepthStencilView(d.dsv.Get(), D3D11_CLEAR_DEPTH, 0.0f, 0);
        RenderScene3D(o, d, scene, gl, frame, sx, sy, 1.0f / refSamples, true, kOutW, kOutH);
        d.ctx->OMSetRenderTargets(1, d.outputRtv.GetAddressOf(), nullptr);
        d.ctx->OMSetBlendState(scene.additive.Get(), nullptr, 0xffffffffu);
        d.ctx->OMSetDepthStencilState(nullptr, 0);
        D3D11_VIEWPORT vpa{0, 0, (float) kOutW, (float) kOutH, 0, 1}; d.ctx->RSSetViewports(1, &vpa);
        d.ctx->VSSetShader(scene.vsBlit.Get(), nullptr, 0);
        d.ctx->PSSetShader(scene.psAccum.Get(), nullptr, 0);
        d.ctx->PSSetShaderResources(0, 1, d.colorSrv.GetAddressOf());
        d.ctx->PSSetSamplers(0, 1, scene.smp.GetAddressOf());
        d.ctx->Draw(3, 0);
        ID3D11ShaderResourceView *nosrv = nullptr; d.ctx->PSSetShaderResources(0, 1, &nosrv);
        d.ctx->OMSetBlendState(nullptr, nullptr, 0xffffffffu);
        d.ctx->OMSetRenderTargets(0, nullptr, nullptr);
    }
}

void PresentBlit(const BenchOptions &o, BenchDevice &d, BenchScene &s, bool paused)
{
    d.ctx->OMSetRenderTargets(1, d.backRtv.GetAddressOf(), nullptr);
    d.ctx->OMSetDepthStencilState(nullptr, 0);
    D3D11_VIEWPORT vpo{0, 0, (float) o.presentW, (float) o.presentH, 0, 1}; d.ctx->RSSetViewports(1, &vpo);
    d.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    d.ctx->VSSetShader(s.vsBlit.Get(), nullptr, 0);
    d.ctx->PSSetShader(s.psBlit.Get(), nullptr, 0);
    d.ctx->PSSetShaderResources(0, 1, paused ? d.colorSrv.GetAddressOf() : d.outputSrv.GetAddressOf());
    d.ctx->PSSetSamplers(0, 1, s.smp.GetAddressOf());
    d.ctx->Draw(3, 0);
    ID3D11ShaderResourceView *nosrv = nullptr; d.ctx->PSSetShaderResources(0, 1, &nosrv);
}
