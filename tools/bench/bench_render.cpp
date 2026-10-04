#include "bench_render.h"

#include "bench_camera.h"
#include "bench_device.h"
#include "bench_gltf_scene.h"
#include "bench_hdri.h"
#include "bench_options.h"
#include "bench_scene.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// Shadow map: an orthographic light frustum fitted to the glTF scene box (a fixed box around
// the camera for the procedural scene), the same light direction as the pixel shader uses.
static void FitShadowFrustum(const BenchOptions &o, const BenchScene &s, const GltfScene &gl, const CameraState &cam, FrameConstants &fc)
{
    const Vec3 lightDir = s.sunDir; // the same variable the pixel shader gets through FrameConstants
    Vec3 lo = gl.gLo, hi = gl.gHi;
    if (o.gltfPath && !gl.gVerts.empty()) {
        // Fit to THIS frame's deformed vertices (gVerts is rebuilt before the shadow pass): the load-time
        // box is the bind pose and a turned/animated head left it, which cut the shadow map off in a
        // straight line across the cheek (2026-09-08). 15% margin plus a floor for thin scenes.
        lo = hi = gl.gVerts[0].pos;
        for (const pwgltf::Vertex &v : gl.gVerts) {
            lo.x = std::min(lo.x, v.pos.x); lo.y = std::min(lo.y, v.pos.y); lo.z = std::min(lo.z, v.pos.z);
            hi.x = std::max(hi.x, v.pos.x); hi.y = std::max(hi.y, v.pos.y); hi.z = std::max(hi.z, v.pos.z);
        }
        const Vec3 c = (lo + hi) * 0.5f; Vec3 e = (hi - lo) * 0.575f;
        const float floorPad = 0.02f * std::max(e.x, std::max(e.y, e.z));
        e.x = std::max(e.x, floorPad); e.y = std::max(e.y, floorPad); e.z = std::max(e.z, floorPad);
        lo = c - e; hi = c + e;
    }
    if (!o.gltfPath) { const Vec3 c{cam.target.x, 0.0f, cam.target.z}; lo = c - Vec3{60, 5, 60}; hi = c + Vec3{60, 40, 60}; }
    const Vec3 centre = (lo + hi) * 0.5f, half = (hi - lo) * 0.5f;
    const float radius = std::sqrt(Dot(half, half));
    const Mat4 lightView = LookAt(centre + lightDir * (radius * 2.0f), centre); // lightDir points at the light, as in the PS
    float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};
    for (int k = 0; k < 8; ++k) {
        const Vec3 p{k & 1 ? hi.x : lo.x, k & 2 ? hi.y : lo.y, k & 4 ? hi.z : lo.z};
        for (int r = 0; r < 3; ++r) {
            const float v = lightView.m[r][0] * p.x + lightView.m[r][1] * p.y + lightView.m[r][2] * p.z + lightView.m[r][3];
            mn[r] = std::min(mn[r], v); mx[r] = std::max(mx[r], v);
        }
    }
    const float pad = 0.02f * radius + 1e-3f;
    // The near/far planes are the box's own extent along the light axis (plus the same 2% pad),
    // so the normalized depth stays tight: env.z below turns a world-unit bias into ndc.
    const float zn = std::max(0.01f, mn[2] - pad), zf = mx[2] + pad;
    fc.lightVp = Mul(Ortho(mn[0] - pad, mx[0] + pad, mn[1] - pad, mx[1] + pad, zn, zf), lightView);
    const float texel = std::max((mx[0] - mn[0]) + 2 * pad, (mx[1] - mn[1]) + 2 * pad) / (float) kShadowSize;
    fc.eyePos[3] = texel; // one shadow texel in world units
    fc.env[2] = zf > zn ? 1.0f / (zf - zn) : 0.0f; // normalized depth per world unit along the light axis
    // --shadow-soft: penumbra half-width as a fraction of the frustum's width. The frustum's
    // width IS kShadowSize texels, so the radius in texels is simply soft * kShadowSize;
    // clamp 1..48 (below 1 the Poisson disk degenerates, above 48 the 16 taps under-sample).
    const float softTexels = std::min(48.0f, std::max(1.0f, o.shadowSoft * (float) kShadowSize));
    fc.env[3] = softTexels;
    static bool s_logShadow = false;
    if (!s_logShadow) {
        s_logShadow = true;
        std::printf("[info] shadow map %ux%u: ortho %.2f x %.2f world units, depth %.2f .. %.2f (range %.2f), texel %.5f world units\n",
                    kShadowSize, kShadowSize, (mx[0] - mn[0]) + 2 * pad, (mx[1] - mn[1]) + 2 * pad, zn, zf, zf - zn, texel);
        std::printf("[info] soft shadows: 16-tap rotated Poisson, --shadow-soft %.4f -> radius %.1f texels = %.4f world units penumbra\n",
                    o.shadowSoft, softTexels, softTexels * texel);
    }
}

// ---- shadow pass: depth only, from the light, same geometry, cull NONE ----
static void DrawShadowPass(const BenchOptions &o, BenchDevice &d, BenchScene &s, GltfScene &gl)
{
    ID3D11ShaderResourceView *noShadow = nullptr; d.ctx->PSSetShaderResources(2, 1, &noShadow); // the SRV cannot stay bound while we write
    d.ctx->ClearDepthStencilView(s.shadowDsv.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
    d.ctx->OMSetRenderTargets(0, nullptr, s.shadowDsv.Get());
    d.ctx->OMSetBlendState(nullptr, nullptr, 0xffffffffu);
    d.ctx->OMSetDepthStencilState(s.dssShadow.Get(), 0);
    d.ctx->RSSetState(s.rs.Get());
    D3D11_VIEWPORT svp{0, 0, (float) kShadowSize, (float) kShadowSize, 0, 1}; d.ctx->RSSetViewports(1, &svp);
    d.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11Buffer *scbs[] = {s.cb.Get(), s.cbInst.Get()};
    d.ctx->VSSetConstantBuffers(0, 2, scbs);
    d.ctx->PSSetShader(nullptr, nullptr, 0);
    if (o.gltfPath) {
        const UINT stride = sizeof(MeshVertex), offset = 0;
        ID3D11Buffer *vb = gl.meshVb.Get();
        d.ctx->IASetInputLayout(s.meshLayout.Get());
        d.ctx->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
        d.ctx->VSSetShader(s.vsMeshShadow.Get(), nullptr, 0);
        // Alpha-tested cut-outs (eyelashes, hair cards) need the texture here too, or they
        // cast solid quads and blotch the face.
        d.ctx->PSSetShader(s.psMeshShadow.Get(), nullptr, 0);
        d.ctx->PSSetSamplers(1, 1, gl.meshSampler.GetAddressOf());
        for (const pwgltf::DrawRange &r : gl.gRanges) {
            ID3D11ShaderResourceView *srv = (r.texture >= 0 && (size_t) r.texture < gl.meshTextures.size() && gl.meshTextures[r.texture]) ? gl.meshTextures[r.texture].Get() : gl.whiteTexture.Get();
            d.ctx->PSSetShaderResources(1, 1, &srv);
            d.ctx->Draw(r.count, r.start);
        }
        ID3D11ShaderResourceView *noTex0 = nullptr; d.ctx->PSSetShaderResources(1, 1, &noTex0);
        d.ctx->PSSetShader(nullptr, nullptr, 0);
        d.ctx->IASetInputLayout(nullptr);
        ID3D11Buffer *noVb = nullptr; const UINT zero0 = 0;
        d.ctx->IASetVertexBuffers(0, 1, &noVb, &zero0, &zero0);
    } else {
        d.ctx->VSSetShader(s.vsGroundShadow.Get(), nullptr, 0); d.ctx->Draw(3, 0);
        d.ctx->VSSetShader(s.vsBoxShadow.Get(), nullptr, 0); d.ctx->DrawInstanced(36, s.instanceCount, 0, 0);
    }
    d.ctx->OMSetRenderTargets(0, nullptr, nullptr);
}

// Colour, motion and depth at the render size: the HDRI sky first, then the glTF meshes or the ground and boxes.
static void DrawScenePass(const BenchOptions &o, BenchDevice &d, BenchScene &s, GltfScene &gl, UINT w, UINT h)
{
    // Without an HDRI the flat sky colour is the clear; with one the sky pass below covers every pixel.
    const float sky[4] = {s.hdriActive ? 0.0f : 0.18f, s.hdriActive ? 0.0f : 0.28f, s.hdriActive ? 0.0f : 0.45f, 1.0f}, zero[4] = {0, 0, 0, 0};
    d.ctx->ClearRenderTargetView(d.colorRtv.Get(), sky);
    d.ctx->ClearRenderTargetView(d.motionRtv.Get(), zero);
    ID3D11RenderTargetView *rts[] = {d.colorRtv.Get(), d.motionRtv.Get()};
    d.ctx->OMSetRenderTargets(2, rts, d.dsv.Get());
    d.ctx->OMSetBlendState(nullptr, nullptr, 0xffffffffu);
    d.ctx->OMSetDepthStencilState(s.dss.Get(), 0);
    d.ctx->RSSetState(s.rs.Get());
    D3D11_VIEWPORT vp{0, 0, (float) w, (float) h, 0, 1}; d.ctx->RSSetViewports(1, &vp);
    d.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11Buffer *cbs[] = {s.cb.Get(), s.cbInst.Get()};
    d.ctx->VSSetConstantBuffers(0, 2, cbs);
    d.ctx->PSSetConstantBuffers(0, 2, cbs);
    if (s.hdriActive) { d.ctx->PSSetShaderResources(3, 1, s.hdriSrv.GetAddressOf()); d.ctx->PSSetSamplers(3, 1, s.hdriSampler.GetAddressOf()); }
    // Sky first, depth off: the fullscreen triangle fills colour and motion, the scene draws over it.
    if (s.hdriActive) {
        d.ctx->OMSetDepthStencilState(s.dssNoDepth.Get(), 0);
        d.ctx->IASetInputLayout(nullptr);
        d.ctx->VSSetShader(s.vsBlit.Get(), nullptr, 0);
        d.ctx->PSSetShader(s.psSky.Get(), nullptr, 0);
        d.ctx->Draw(3, 0);
        d.ctx->OMSetDepthStencilState(s.dss.Get(), 0);
    }
    d.ctx->PSSetShader(s.psScene.Get(), nullptr, 0);
    d.ctx->PSSetShaderResources(2, 1, s.shadowSrv.GetAddressOf());
    d.ctx->PSSetSamplers(2, 1, s.shadowSmp.GetAddressOf());
    if (o.gltfPath) {
        const UINT stride = sizeof(MeshVertex), offset = 0;
        ID3D11Buffer *vb = gl.meshVb.Get();
        d.ctx->IASetInputLayout(s.meshLayout.Get());
        d.ctx->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
        d.ctx->VSSetShader(s.vsMesh.Get(), nullptr, 0);
        d.ctx->PSSetSamplers(1, 1, gl.meshSampler.GetAddressOf());
        for (const pwgltf::DrawRange &r : gl.gRanges) {
            ID3D11ShaderResourceView *srv = (r.texture >= 0 && (size_t) r.texture < gl.meshTextures.size() && gl.meshTextures[r.texture]) ? gl.meshTextures[r.texture].Get() : gl.whiteTexture.Get();
            d.ctx->PSSetShaderResources(1, 1, &srv);
            d.ctx->Draw(r.count, r.start);
        }
        ID3D11ShaderResourceView *noTex = nullptr; d.ctx->PSSetShaderResources(1, 1, &noTex);
        d.ctx->IASetInputLayout(nullptr);
        ID3D11Buffer *noVb = nullptr; const UINT zero0 = 0;
        d.ctx->IASetVertexBuffers(0, 1, &noVb, &zero0, &zero0);
    } else {
        static const bool noGround = GetEnvironmentVariableA("PW_BENCH_NO_GROUND", nullptr, 0) != 0; // debug
        if (!noGround) { d.ctx->VSSetShader(s.vsGround.Get(), nullptr, 0); d.ctx->Draw(3, 0); }
        d.ctx->VSSetShader(s.vsBox.Get(), nullptr, 0);
        d.ctx->DrawInstanced(36, s.instanceCount, 0, 0);
    }
    ID3D11RenderTargetView *none[2] = {}; d.ctx->OMSetRenderTargets(2, none, nullptr);
    ID3D11ShaderResourceView *noShadow = nullptr; d.ctx->PSSetShaderResources(2, 1, &noShadow);
    d.ctx->OMSetBlendState(nullptr, nullptr, 0xffffffffu);
}

void RenderScene3D(const BenchOptions &o, BenchDevice &d, BenchScene &s, GltfScene &gl, int frame, float jx, float jy, float weight, bool additiveColor, UINT w, UINT h)
{
    float fovY = 60.0f;
    const CameraState cam = ChooseCamera(o, gl.gscene, frame, fovY);
    const float aspect = (float) w / (float) h;
    const Mat4 view = LookAt(cam.eye, cam.target);
    // --cam-shift: Blender's camera shift_x / shift_y, in units of the FITTED sensor dimension
    // (the frame's height for the "VERTICAL" sensor fit the .blend uses). A shift moves the frame,
    // so the content moves the other way; measured against Blender: content_dx = -shift_x * height
    // and content_dy(down) = +shift_y * height pixels, which is exactly what the projection's
    // jitter offset expresses. It goes into the unjittered matrices too - it is part of the
    // camera, not of the sampling pattern - so the motion vectors stay correct.
    const float shiftX = -o.camShift.x * (float) w, shiftY = o.camShift.y * (float) w; // Blender: shift is a fraction of the larger frame dimension
    const Mat4 projJ = Perspective(fovY, aspect, 0.1f, 5000.0f, jx + shiftX, jy + shiftY, (float) w, (float) h);
    const Mat4 proj = Perspective(fovY, aspect, 0.1f, 5000.0f, shiftX, shiftY, (float) w, (float) h);
    FrameConstants fc{};
    fc.vp = Mul(projJ, view);
    fc.vpNoJitter = Mul(proj, view);
    fc.vpPrev = s.havePrev ? s.vpPrevNoJitter : fc.vpNoJitter;
    FitShadowFrustum(o, s, gl, cam, fc);
    fc.renderSize[0] = (float) w; fc.renderSize[1] = (float) h; fc.renderSize[2] = o.mvDir; fc.renderSize[3] = 1.0f / o.mvScale;
    fc.invVp = Invert(fc.vp);
    fc.lightDir[0] = s.sunDir.x; fc.lightDir[1] = s.sunDir.y; fc.lightDir[2] = s.sunDir.z; fc.lightDir[3] = s.hdriActive ? (g_hdriMirror ? 2.0f : 1.0f) : 0.0f;
    fc.sunColor[0] = s.sunCol[0]; fc.sunColor[1] = s.sunCol[1]; fc.sunColor[2] = s.sunCol[2]; fc.sunColor[3] = s.hdriScale * o.exposure;
    fc.hdriParams[0] = 2.5f * g_hdriMeanL; fc.hdriParams[1] = o.ambientScale; fc.hdriParams[2] = o.albedoScale; fc.hdriParams[3] = 0.0f;
    fc.env[0] = g_hdriYawCos; fc.env[1] = g_hdriYawSin; // env.z / env.w are set with the shadow frustum above
    fc.lab[0] = (float) gl.gLights.size(); fc.lab[1] = o.hdrOutput ? 1.0f : 0.0f; fc.lab[2] = o.boil; fc.lab[3] = g_standardDepth ? 1.0f : 0.0f;
    for (size_t li = 0; li < gl.gLights.size(); ++li) {
        fc.lightPos[li][0] = gl.gLights[li].pos.x; fc.lightPos[li][1] = gl.gLights[li].pos.y; fc.lightPos[li][2] = gl.gLights[li].pos.z; fc.lightPos[li][3] = 1.0f;
        // radiance x area / pi^0: a Lambertian emitter of area A and radiance L gives L * A / d^2 (times the
        // cosine at the emitter, dropped: the tubes are seen from below and around alike).
        for (int k = 0; k < 3; ++k) fc.lightPower[li][k] = gl.gLights[li].power[k] * o.exposure;
    }
    fc.misc[0] = weight; fc.misc[1] = frame / 60.0f; fc.misc[3] = o.exposure;
    fc.eyePos[0] = cam.eye.x; fc.eyePos[1] = cam.eye.y; fc.eyePos[2] = cam.eye.z;
    static const bool idColours = GetEnvironmentVariableA("PW_BENCH_ID_COLOURS", nullptr, 0) != 0; // debug
    static const float dbgMode = [] { char v[8] = {}; return GetEnvironmentVariableA("PW_BENCH_ID_COLOURS", v, sizeof(v)) ? (float) std::max(1, atoi(v)) : 0.0f; }();
    fc.misc[2] = dbgMode;
    D3D11_MAPPED_SUBRESOURCE m{};
    d.ctx->Map(s.cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m); memcpy(m.pData, &fc, sizeof(fc)); d.ctx->Unmap(s.cb.Get(), 0);
    // Object motion: the two last boxes bob and circle; previous positions carry the object MVs.
    if (o.movingBoxes) AnimateBoxes(s, frame);
    d.ctx->Map(s.cbInst.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m); memcpy(m.pData, &s.inst, sizeof(s.inst)); d.ctx->Unmap(s.cbInst.Get(), 0);
    if (o.movingBoxes) { // restore base positions for the next frame's "previous"
        static float base2[2][4]; (void) base2;
    }
    (void) additiveColor;
    if (o.gltfPath) UploadMesh(o, d, gl, frame * o.animSpeed / 60.0f);
    DrawShadowPass(o, d, s, gl);
    DrawScenePass(o, d, s, gl, w, h);
    s.vpPrevNoJitter = fc.vpNoJitter;
    s.havePrev = true;
}
