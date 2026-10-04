// The window, the D3D11 device and swap chain, and the render-size / output targets DLSS works on.
#pragma once
#include "bench_d3d.h"

#include <dxgi.h>

struct BenchOptions;

struct BenchDevice {
    HWND hwnd = nullptr;
    ComPtr<ID3D11Device> dev; ComPtr<ID3D11DeviceContext> ctx; ComPtr<IDXGISwapChain> sc;
    // Render-resolution targets (or the reference's 4K targets), the DLSS output, the swap chain's back buffer.
    ComPtr<ID3D11Texture2D> color, motion, depth, output, back;
    ComPtr<ID3D11RenderTargetView> colorRtv, motionRtv, outputRtv, backRtv; ComPtr<ID3D11DepthStencilView> dsv; ComPtr<ID3D11ShaderResourceView> outputSrv, colorSrv;
};

// D3D12 debug layer (--debug-layer / PW_BENCH_DEBUG_LAYER), window, device, swap chain and targets;
// false after printing the failure.
bool CreateBenchDevice(const BenchOptions &o, BenchDevice &d);
// --debug-layer: lists the D3D11 debug device's warnings and worse, and the message counts.
void ReportD3D11Messages(const BenchOptions &o, BenchDevice &d);
