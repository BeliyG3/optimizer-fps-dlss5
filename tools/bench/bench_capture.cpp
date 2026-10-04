#include "bench_capture.h"

#include "bench_device.h"
#include "bench_options.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

bool CreateCaptureTargets(const BenchOptions &o, BenchDevice &d, Capture &c)
{
    if (o.measureEvery > 0 || !o.dumpFrames.empty()) {
        D3D11_TEXTURE2D_DESC sdesc{}; sdesc.Width = kOutW; sdesc.Height = kOutH; sdesc.MipLevels = 1; sdesc.ArraySize = 1;
        sdesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; sdesc.SampleDesc.Count = 1; sdesc.Usage = D3D11_USAGE_STAGING; sdesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        d.dev->CreateTexture2D(&sdesc, nullptr, &c.staging);
        if (!c.staging) { std::printf("[fail] staging texture\n"); return false; }
    }
    if (o.dumpBack) {
        D3D11_TEXTURE2D_DESC bdesc{}; d.back->GetDesc(&bdesc);
        bdesc.BindFlags = 0; bdesc.MiscFlags = 0; bdesc.Usage = D3D11_USAGE_STAGING; bdesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(d.dev->CreateTexture2D(&bdesc, nullptr, &c.backStaging))) { std::printf("[fail] back buffer staging texture\n"); return false; }
    }
    return true;
}

static float HalfToFloat(unsigned short h)
{
    unsigned int sign = (h >> 15) & 1u, exp = (h >> 10) & 0x1Fu, mant = h & 0x3FFu;
    float v;
    if (exp == 0) v = std::ldexp((float) mant, -24);
    else if (exp == 31) v = mant ? NAN : INFINITY;
    else v = std::ldexp((float) (mant | 0x400u), (int) exp - 25);
    return sign ? -v : v;
}

void MeasureOutput(BenchDevice &d, Capture &c, int frameIndex)
{
    d.ctx->CopyResource(c.staging.Get(), d.output.Get());
    D3D11_MAPPED_SUBRESOURCE map{};
    if (FAILED(d.ctx->Map(c.staging.Get(), 0, D3D11_MAP_READ, 0, &map))) { std::printf("[warn] measure map failed\n"); return; }
    double all = 0, center = 0, periphery = 0; long long nAll = 0, nCenter = 0, nPer = 0;
    const UINT cx0 = kOutW / 5, cx1 = kOutW - kOutW / 5, cy0 = kOutH / 5, cy1 = kOutH - kOutH / 5;
    for (UINT y = 0; y < kOutH; y += 4) {
        const unsigned short *row = (const unsigned short *) ((const char *) map.pData + (size_t) y * map.RowPitch);
        for (UINT x = 0; x < kOutW; x += 4) {
            const float r = HalfToFloat(row[x * 4 + 0]), g = HalfToFloat(row[x * 4 + 1]), b = HalfToFloat(row[x * 4 + 2]);
            const double luma = 0.2126 * r + 0.7152 * g + 0.0722 * b;
            all += luma; ++nAll;
            if (x >= cx0 && x < cx1 && y >= cy0 && y < cy1) { center += luma; ++nCenter; } else { periphery += luma; ++nPer; }
        }
    }
    d.ctx->Unmap(c.staging.Get(), 0);
    all /= nAll; center /= nCenter; periphery /= nPer;
    c.sumAll += all; c.sumCenter += center; c.sumPeriphery += periphery; ++c.measurements;
    std::printf("[measure] frame %d: mean luma all=%.5f center=%.5f periphery=%.5f\n", frameIndex, all, center, periphery);
}

// --dump N: the output of frame N, full size, as dump_N.bmp beside the exe. The scene shaders
// already tone map to linear 0..1, so the only step left here is the linear -> sRGB encode.
void DumpOutput(BenchDevice &d, Capture &c, int frameIndex)
{
    d.ctx->CopyResource(c.staging.Get(), d.output.Get());
    D3D11_MAPPED_SUBRESOURCE map{};
    if (FAILED(d.ctx->Map(c.staging.Get(), 0, D3D11_MAP_READ, 0, &map))) { std::printf("[warn] dump map failed\n"); return; }
    const UINT w = kOutW, h = kOutH;
    const UINT rowBytes = ((w * 3 + 3) / 4) * 4;
    std::vector<unsigned char> pixels((size_t) rowBytes * h);
    for (UINT y = 0; y < h; ++y) {
        const unsigned short *row = (const unsigned short *) ((const char *) map.pData + (size_t) y * map.RowPitch);
        unsigned char *out = pixels.data() + (size_t) (h - 1 - y) * rowBytes;
        for (UINT x = 0; x < w; ++x) {
            for (int ch = 0; ch < 3; ++ch) {
                float v = HalfToFloat(row[x * 4 + ch]);
                v = v < 0 ? 0 : (v > 1 ? 1 : v);
                v = v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f; // linear -> sRGB
                out[x * 3 + (2 - ch)] = (unsigned char) (std::min(1.0f, v) * 255.0f + 0.5f);
            }
        }
    }
    d.ctx->Unmap(c.staging.Get(), 0);
    char name[64]; std::snprintf(name, sizeof(name), "dump_%d.bmp", frameIndex);
    FILE *fp = nullptr; fopen_s(&fp, name, "wb");
    if (!fp) { std::printf("[warn] dump open failed\n"); return; }
    const unsigned int fileSize = 54 + (unsigned int) pixels.size();
    unsigned char header[54] = {'B', 'M'};
    memcpy(header + 2, &fileSize, 4); const unsigned int off = 54; memcpy(header + 10, &off, 4);
    const unsigned int dib = 40; memcpy(header + 14, &dib, 4); memcpy(header + 18, &w, 4); memcpy(header + 22, &h, 4);
    const unsigned short planes = 1, bpp = 24; memcpy(header + 26, &planes, 2); memcpy(header + 28, &bpp, 2);
    const unsigned int imageSize = (unsigned int) pixels.size(); memcpy(header + 34, &imageSize, 4);
    fwrite(header, 1, 54, fp); fwrite(pixels.data(), 1, pixels.size(), fp); fclose(fp);
    std::printf("[info] frame %d dumped to %s (%ux%u)\n", frameIndex, name, w, h);
}

// --dump-back: the back buffer before Present (what the present event sees), in the add-on's menu dump layout.
void DumpBackBuffer(const BenchOptions &o, BenchDevice &d, Capture &c, int frameIndex)
{
    d.ctx->CopyResource(c.backStaging.Get(), d.back.Get());
    D3D11_MAPPED_SUBRESOURCE map{};
    if (FAILED(d.ctx->Map(c.backStaging.Get(), 0, D3D11_MAP_READ, 0, &map))) { std::printf("[warn] back buffer map failed\n"); return; }
    std::vector<unsigned char> pixels((size_t) o.presentW * o.presentH * 4);
    for (UINT y = 0; y < o.presentH; ++y) {
        const unsigned char *src = (const unsigned char *) map.pData + (size_t) y * map.RowPitch;
        unsigned char *dst = pixels.data() + (size_t) y * o.presentW * 4;
        for (UINT x = 0; x < o.presentW; ++x) { dst[x * 4] = src[x * 4 + 2]; dst[x * 4 + 1] = src[x * 4 + 1]; dst[x * 4 + 2] = src[x * 4]; dst[x * 4 + 3] = 255; }
    }
    d.ctx->Unmap(c.backStaging.Get(), 0);
    char name[64]; std::snprintf(name, sizeof(name), "dump_%d.bmp", frameIndex);
    FILE *fp = nullptr; fopen_s(&fp, name, "wb");
    if (!fp) { std::printf("[warn] dump open failed\n"); return; }
    BITMAPFILEHEADER file{}; file.bfType = 0x4d42; file.bfOffBits = sizeof(file) + sizeof(BITMAPINFOHEADER); file.bfSize = file.bfOffBits + (DWORD) pixels.size();
    BITMAPINFOHEADER info{}; info.biSize = sizeof(info); info.biWidth = (LONG) o.presentW; info.biHeight = -(LONG) o.presentH;
    info.biPlanes = 1; info.biBitCount = 32; info.biCompression = BI_RGB; info.biSizeImage = (DWORD) pixels.size();
    fwrite(&file, sizeof(file), 1, fp); fwrite(&info, sizeof(info), 1, fp); fwrite(pixels.data(), 1, pixels.size(), fp); fclose(fp);
    std::printf("[info] frame %d back buffer dumped to %s (%ux%u)\n", frameIndex, name, o.presentW, o.presentH);
}

void PrintMeasureAverage(const Capture &c)
{
    if (c.measurements > 0)
        std::printf("[measure] average over %d samples: all=%.5f center=%.5f periphery=%.5f\n", c.measurements,
                    c.sumAll / c.measurements, c.sumCenter / c.measurements, c.sumPeriphery / c.measurements);
}
