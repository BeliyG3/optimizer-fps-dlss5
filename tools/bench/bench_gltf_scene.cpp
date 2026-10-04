#include "bench_gltf_scene.h"

#include "bench_device.h"
#include "bench_options.h"

#include <wincodec.h> // decoding the textures embedded in a .glb (WIC)

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>

bool LoadGltf(BenchOptions &o, GltfScene &gl)
{
    if (o.flat) { std::printf("[fail] --gltf needs the 3D scene\n"); return false; }
    if (!pwgltf::Load(o.gltfPath, gl.gscene)) { std::printf("[fail] glTF: %s (%s)\n", gl.gscene.error.c_str(), o.gltfPath); return false; }
    size_t tris = 0; for (const auto &m : gl.gscene.meshes) for (const auto &p : m.primitives) tris += p.indices.size() / 3;
    std::printf("[info] glTF %s: %zu nodes, %zu meshes, %zu triangles, %zu animation channels (%.2f s), camera %s\n", o.gltfPath, gl.gscene.nodes.size(),
                gl.gscene.meshes.size(), tris, gl.gscene.channels.size(), gl.gscene.animationEnd, gl.gscene.cameraNode >= 0 ? gl.gscene.nodes[gl.gscene.cameraNode].name.c_str() : "none");
    if (!o.cameraGiven && gl.gscene.cameraNode >= 0) o.cameraMode = "file";
    if (!o.cameraGiven && gl.gscene.cameraNode < 0) std::printf("[info] glTF has no camera: the scripted camera path is used\n");
    return true;
}

bool CreateGltfResources(BenchDevice &d, GltfScene &gl)
{
    D3D11_SAMPLER_DESC msd{}; msd.Filter = D3D11_FILTER_ANISOTROPIC; msd.MaxAnisotropy = 8; msd.AddressU = msd.AddressV = msd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP; msd.MaxLOD = D3D11_FLOAT32_MAX;
    d.dev->CreateSamplerState(&msd, &gl.meshSampler);
    auto makeTexture = [&](const uint8_t *rgba, UINT w, UINT h, bool srgb) -> ComPtr<ID3D11ShaderResourceView> {
        D3D11_TEXTURE2D_DESC td{}; td.Width = w; td.Height = h; td.MipLevels = 0; td.ArraySize = 1;
        td.Format = srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET; td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
        ComPtr<ID3D11Texture2D> tex; if (FAILED(d.dev->CreateTexture2D(&td, nullptr, &tex))) return nullptr;
        d.ctx->UpdateSubresource(tex.Get(), 0, nullptr, rgba, w * 4, 0);
        ComPtr<ID3D11ShaderResourceView> srv; if (FAILED(d.dev->CreateShaderResourceView(tex.Get(), nullptr, &srv))) return nullptr;
        d.ctx->GenerateMips(srv.Get());
        return srv;
    };
    const uint8_t white[4] = {255, 255, 255, 255};
    gl.whiteTexture = makeTexture(white, 1, 1, false);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ComPtr<IWICImagingFactory> wic;
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) {
        for (const pwgltf::Image &img : gl.gscene.images) {
            ComPtr<ID3D11ShaderResourceView> srv;
            ComPtr<IWICStream> stream; ComPtr<IWICBitmapDecoder> decoder; ComPtr<IWICBitmapFrameDecode> frame0; ComPtr<IWICFormatConverter> conv;
            if (!img.bytes.empty() && SUCCEEDED(wic->CreateStream(&stream)) &&
                SUCCEEDED(stream->InitializeFromMemory(const_cast<BYTE *>(img.bytes.data()), (DWORD) img.bytes.size())) &&
                SUCCEEDED(wic->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) &&
                SUCCEEDED(decoder->GetFrame(0, &frame0)) && SUCCEEDED(wic->CreateFormatConverter(&conv)) &&
                SUCCEEDED(conv->Initialize(frame0.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
                UINT w = 0, h = 0; conv->GetSize(&w, &h);
                std::vector<uint8_t> rgba((size_t) w * h * 4);
                if (w && h && SUCCEEDED(conv->CopyPixels(nullptr, w * 4, (UINT) rgba.size(), rgba.data()))) srv = makeTexture(rgba.data(), w, h, true);
            }
            if (!srv) std::printf("[warn] glTF image (%s, %zu bytes) could not be decoded; white is used\n", img.mime.c_str(), img.bytes.size());
            gl.meshTextures.push_back(srv);
        }
    } else std::printf("[warn] WIC unavailable: glTF textures are not used\n");
    std::printf("[info] glTF textures: %zu image(s), %zu decoded\n", gl.gscene.images.size(), (size_t) std::count_if(gl.meshTextures.begin(), gl.meshTextures.end(), [](const ComPtr<ID3D11ShaderResourceView> &s) { return s != nullptr; }));
    pwgltf::BuildVertices(gl.gscene, 0.0f, 1.0f / 60.0f, gl.gVerts, gl.gPrev, &gl.gRanges, &gl.gLights);
    if (gl.gVerts.empty()) { std::printf("[fail] glTF: no triangles\n"); return false; }
    if (gl.gLights.size() > 24) {
        // The brightest 24: the shader's array is that long.
        std::sort(gl.gLights.begin(), gl.gLights.end(), [](const pwgltf::Light &a, const pwgltf::Light &b) { return a.power[0] + a.power[1] + a.power[2] > b.power[0] + b.power[1] + b.power[2]; });
        gl.gLights.resize(24);
    }
    std::printf("[info] glTF lights: %zu emissive surface(s) used as point lights\n", gl.gLights.size());
    gl.gLo = gl.gHi = gl.gVerts[0].pos;
    for (const pwgltf::Vertex &v : gl.gVerts) {
        gl.gLo.x = std::min(gl.gLo.x, v.pos.x); gl.gLo.y = std::min(gl.gLo.y, v.pos.y); gl.gLo.z = std::min(gl.gLo.z, v.pos.z);
        gl.gHi.x = std::max(gl.gHi.x, v.pos.x); gl.gHi.y = std::max(gl.gHi.y, v.pos.y); gl.gHi.z = std::max(gl.gHi.z, v.pos.z);
    }
    const Vec3 c = (gl.gLo + gl.gHi) * 0.5f, e = (gl.gHi - gl.gLo) * 0.6f; // 20% margin: morphs and animation move vertices
    gl.gLo = c - e; gl.gHi = c + e;
    std::printf("[info] glTF bounds (%.1f, %.1f, %.1f) .. (%.1f, %.1f, %.1f)\n", gl.gLo.x, gl.gLo.y, gl.gLo.z, gl.gHi.x, gl.gHi.y, gl.gHi.z);
    D3D11_BUFFER_DESC vbd{}; vbd.Usage = D3D11_USAGE_DYNAMIC; vbd.BindFlags = D3D11_BIND_VERTEX_BUFFER; vbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    vbd.ByteWidth = (UINT) (gl.gVerts.size() * sizeof(MeshVertex));
    if (FAILED(d.dev->CreateBuffer(&vbd, nullptr, &gl.meshVb))) { std::printf("[fail] glTF vertex buffer\n"); return false; }
    gl.gGpu.resize(gl.gVerts.size());
    return true;
}

void UploadMesh(const BenchOptions &o, BenchDevice &d, GltfScene &gl, float t)
{
    const auto t0 = std::chrono::high_resolution_clock::now();
    pwgltf::BuildVertices(gl.gscene, t, o.animSpeed / 60.0f, gl.gVerts, gl.gPrev, &gl.gRanges);
    gl.skinMsTotal += std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - t0).count();
    ++gl.skinFrames;
    for (size_t i = 0; i < gl.gVerts.size() && i < gl.gGpu.size(); ++i) {
        const pwgltf::Vertex &v = gl.gVerts[i]; MeshVertex &g = gl.gGpu[i];
        g.pos[0] = v.pos.x; g.pos[1] = v.pos.y; g.pos[2] = v.pos.z;
        g.prev[0] = gl.gPrev[i].x; g.prev[1] = gl.gPrev[i].y; g.prev[2] = gl.gPrev[i].z;
        g.normal[0] = v.normal.x; g.normal[1] = v.normal.y; g.normal[2] = v.normal.z;
        memcpy(g.colour, v.colour, sizeof(g.colour));
        const bool textured = v.texture >= 0 && (size_t) v.texture < gl.meshTextures.size() && gl.meshTextures[v.texture];
        g.misc[0] = v.material < -1.5f && v.stripe > 0.5f ? -3.0f : v.material; g.misc[1] = v.localY; g.misc[2] = textured ? (v.alphaMask ? 2.0f + v.alphaCutoff : 1.0f) : 0.0f;
        g.uv[0] = v.uv[0]; g.uv[1] = v.uv[1];
    }
    D3D11_MAPPED_SUBRESOURCE m{};
    if (SUCCEEDED(d.ctx->Map(gl.meshVb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) { memcpy(m.pData, gl.gGpu.data(), gl.gGpu.size() * sizeof(MeshVertex)); d.ctx->Unmap(gl.meshVb.Get(), 0); }
}
