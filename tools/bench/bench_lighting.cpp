#include "bench_scene.h"

#include "bench_device.h"
#include "bench_hdri.h"
#include "bench_options.h"
#include "bench_shaders.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

void CreateLighting(BenchOptions &o, BenchDevice &d, BenchScene &s)
{
    // ---- environment lighting from an equirect HDRI (--hdri): sky, ambient, sun ----
    if (!o.flat) {
        std::string chosen;
        if (o.hdriPath && strcmp(o.hdriPath, "none") == 0) chosen.clear();
        else if (o.hdriPath) chosen = o.hdriPath;
        else {
            static const char *const candidates[] = {"..\\assets\\external\\golden_gate_hills_2k.hdr",
                                                     "assets\\external\\golden_gate_hills_2k.hdr",
                                                     "..\\..\\assets\\external\\golden_gate_hills_2k.hdr"};
            for (const char *c : candidates) { FILE *f = nullptr; fopen_s(&f, c, "rb"); if (f) { fclose(f); chosen = c; break; } }
        }
        HdrImage hdr;
        if (!chosen.empty() && !LoadHdr(chosen.c_str(), hdr)) std::printf("[warn] hdri %s could not be read; the flat sky/ambient is used\n", chosen.c_str());
        else if (!chosen.empty()) {
            // The map is used at its raw radiance (hdriScale stays 1): the mean luminance below is
            // only printed for information and used as the fallback reference for the sun strength.
            double sumL = 0, sumW = 0;
            std::vector<float> luma((size_t) hdr.w * hdr.h);
            for (int y = 0; y < hdr.h; ++y) {
                const double sw = std::sin((y + 0.5) / hdr.h * 3.14159265);
                for (int x = 0; x < hdr.w; ++x) {
                    const float *p = &hdr.rgb[((size_t) y * hdr.w + x) * 3];
                    const float l = 0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2];
                    luma[(size_t) y * hdr.w + x] = l;
                    sumL += l * sw; sumW += sw;
                }
            }
            const double meanL = sumW > 0 ? sumL / sumW : 1.0; // solid-angle weighted mean radiance (luma)
            g_hdriMeanL = (float) meanL;
            // Sun: centroid of the brightest 0.01% of the pixels, weighted by luminance.
            std::vector<float> sorted = luma;
            const size_t topN = std::max<size_t>(16, luma.size() / 10000);
            std::nth_element(sorted.begin(), sorted.end() - topN, sorted.end());
            const float threshold = *(sorted.end() - topN);
            // Their radiance integrated over their solid angle is the sun irradiance E_sun; the
            // shader wants E_sun / pi (see the Lambert convention in the PS), so divide it there.
            Vec3 acc{0, 0, 0}; double irrR = 0, irrG = 0, irrB = 0; size_t hits = 0;
            const double dPhi = 6.2831853 / hdr.w, dTheta = 3.14159265 / hdr.h;
            for (int y = 0; y < hdr.h; ++y) {
                const double dOmega = std::sin((y + 0.5) / hdr.h * 3.14159265) * dPhi * dTheta;
                for (int x = 0; x < hdr.w; ++x) {
                    const float l = luma[(size_t) y * hdr.w + x];
                    if (l < threshold) continue;
                    const Vec3 d = HdrUvToDir(g_hdriMirror ? 1.0f - (x + 0.5f) / hdr.w : (x + 0.5f) / hdr.w, (y + 0.5f) / hdr.h);
                    acc = acc + d * l;
                    const float *p = &hdr.rgb[((size_t) y * hdr.w + x) * 3];
                    irrR += p[0] * dOmega; irrG += p[1] * dOmega; irrB += p[2] * dOmega; ++hits;
                }
            }
            if (hits > 0 && Dot(acc, acc) > 0) {
                s.sunDir = Normalize(acc); // still in HDRI space; rotated by the yaw below
                // E_sun / pi, i.e. the same units as the ambient average radiance the shader reads.
                float r = (float) (irrR / 3.14159265), g = (float) (irrG / 3.14159265), b = (float) (irrB / 3.14159265);
                const float l = 0.2126f * r + 0.7152f * g + 0.0722f * b;
                // The measured disk is unusable as an absolute: a 2k .hdr clips it (under-reads) while a
                // wide bright sky over-reads it. Keep the measured chroma and normalise the sun to
                // kSunOverSky x the sky's mean radiance, which is the sun/sky ratio knob: lit surfaces
                // land ~(1 + kSunOverSky)x above shadowed ones. 1.2 is a soft, nearly shadowless key
                // (the old 3.0 gave a hard sunlit look with black shadow sides). --sun-scale overrides it.
                const float kSunOverSky = 1.2f;
                const float ratio = o.sunScale > 0.0f ? o.sunScale : kSunOverSky;
                const float target = (float) (ratio * meanL);
                const float k = l > 1e-6f ? target / l : 1.0f;
                r *= k; g *= k; b *= k;
                std::printf("[info] hdri sun: measured E/pi luma %.3f -> %.3f (%.2fx sky mean %.3f)\n", l, target, ratio, meanL);
                s.sunCol[0] = r; s.sunCol[1] = g; s.sunCol[2] = b;
            }
            // --hdri-yaw: pick the rotation, then turn the sun with the sky (the only value that comes
            // out of the map in HDRI space). Auto aims the sun kSunOffCameraDeg to the side of the face
            // camera's axis: a front-side key light instead of whatever the map happened to point at.
            {
                const float kSunOffCameraDeg = 40.0f; // sun azimuth - camera azimuth, for --camera face
                const float sunAz = std::atan2(s.sunDir.x, s.sunDir.z) * (180.0f / 3.14159265f);
                const float sunEl = std::asin(std::max(-1.0f, std::min(1.0f, s.sunDir.y))) * (180.0f / 3.14159265f);
                const bool faceCam = strcmp(o.cameraMode, "face") == 0;
                if (o.hdriYawAuto) o.hdriYaw = faceCam ? o.faceYaw + o.faceAngle + kSunOffCameraDeg - sunAz : o.sceneYaw;
                const float rad = o.hdriYaw * 3.14159265f / 180.0f;
                g_hdriYawCos = std::cos(rad); g_hdriYawSin = std::sin(rad);
                s.sunDir = RotateHdriYaw(s.sunDir);
                std::printf("[info] hdri yaw %.1f deg (%s): sun azimuth %.1f -> %.1f deg, elevation %.1f deg", o.hdriYaw, o.hdriYawAuto ? "auto" : "given", sunAz, sunAz + o.hdriYaw, sunEl);
                if (faceCam) std::printf("; face camera axis %.1f deg, sun %.1f deg to its side", o.faceYaw + o.faceAngle, sunAz + o.hdriYaw - (o.faceYaw + o.faceAngle));
                std::printf("\n");
            }
            // R16G16B16A16_FLOAT with a full mip chain: mip 8 (8x4) is the diffuse irradiance lookup.
            std::vector<unsigned short> half((size_t) hdr.w * hdr.h * 4);
            for (size_t i = 0; i < (size_t) hdr.w * hdr.h; ++i) {
                half[i * 4 + 0] = FloatToHalf(hdr.rgb[i * 3 + 0]);
                half[i * 4 + 1] = FloatToHalf(hdr.rgb[i * 3 + 1]);
                half[i * 4 + 2] = FloatToHalf(hdr.rgb[i * 3 + 2]);
                half[i * 4 + 3] = FloatToHalf(1.0f);
            }
            D3D11_TEXTURE2D_DESC td{}; td.Width = hdr.w; td.Height = hdr.h; td.MipLevels = 0; td.ArraySize = 1;
            td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET; td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
            ComPtr<ID3D11Texture2D> hdriTex;
            if (SUCCEEDED(d.dev->CreateTexture2D(&td, nullptr, &hdriTex))) {
                d.ctx->UpdateSubresource(hdriTex.Get(), 0, nullptr, half.data(), hdr.w * 8, 0);
                if (SUCCEEDED(d.dev->CreateShaderResourceView(hdriTex.Get(), nullptr, &s.hdriSrv))) d.ctx->GenerateMips(s.hdriSrv.Get());
            }
            D3D11_SAMPLER_DESC hsd{}; hsd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
            hsd.AddressU = D3D11_TEXTURE_ADDRESS_WRAP; hsd.AddressV = hsd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP; hsd.MaxLOD = D3D11_FLOAT32_MAX;
            d.dev->CreateSamplerState(&hsd, &s.hdriSampler);
            ComPtr<ID3DBlob> b = Compile(kShader3D, "PSSky", "ps_5_0");
            if (b) d.dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &s.psSky);
            s.hdriActive = s.hdriSrv && s.hdriSampler && s.psSky;
            if (!s.hdriActive) std::printf("[warn] hdri resources could not be created; the flat sky/ambient is used\n");
            else {
                std::printf("[info] hdri %s: %dx%d, mean radiance (luma) %.3f, scale %.3f, exposure %.2f\n", chosen.c_str(), hdr.w, hdr.h, meanL, s.hdriScale, o.exposure);
                std::printf("[info] hdri sun dir (%.3f, %.3f, %.3f) colour (%.3f, %.3f, %.3f)\n", s.sunDir.x, s.sunDir.y, s.sunDir.z, s.sunCol[0], s.sunCol[1], s.sunCol[2]);
            }
        }
    }
    // ---- --sun-dir / --sun-strength: the Blender lamp instead of the sun extracted from the HDRI ----
    // Both bypass the extraction above (and kSunOverSky / --sun-scale) completely: the direction is
    // already in world space, so it is NOT turned by --hdri-yaw, and the strength is an irradiance E
    // in the same radiance units as the HDRI at strength 1, which the shader wants as E / pi.
    if (o.sunDirGiven) {
        s.sunDir = Normalize(Vec3{-o.sunDirTravel.x, -o.sunDirTravel.y, -o.sunDirTravel.z}); // travels toward -> points at the light
        std::printf("[info] sun dir override: travels (%.4f, %.4f, %.4f) -> lightDir (%.4f, %.4f, %.4f), azimuth %.1f deg, elevation %.1f deg\n",
                    o.sunDirTravel.x, o.sunDirTravel.y, o.sunDirTravel.z, s.sunDir.x, s.sunDir.y, s.sunDir.z,
                    std::atan2(s.sunDir.x, s.sunDir.z) * (180.0f / 3.14159265f),
                    std::asin(std::max(-1.0f, std::min(1.0f, s.sunDir.y))) * (180.0f / 3.14159265f));
    }
    if (o.sunStrengthGiven) {
        const float e = o.sunStrength / 3.14159265f;
        s.sunCol[0] = s.sunCol[1] = s.sunCol[2] = e;
        std::printf("[info] sun strength override: E %.3f -> E/pi %.4f (white)\n", o.sunStrength, e);
    }
    // Depth off for the sky pass (the cleared far depth stays, the scene draws over it).
    { D3D11_DEPTH_STENCIL_DESC nd{}; nd.DepthEnable = FALSE; nd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO; nd.DepthFunc = D3D11_COMPARISON_ALWAYS; d.dev->CreateDepthStencilState(&nd, &s.dssNoDepth); }
}
