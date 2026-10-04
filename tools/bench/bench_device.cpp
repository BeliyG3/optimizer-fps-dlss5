#include "bench_device.h"

#include "bench_options.h"

#include <d3d12.h>

#include <cstdio>
#include <vector>

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(h, m, w, l);
}

bool CreateBenchDevice(const BenchOptions &o, BenchDevice &d)
{
    if (o.debugLayer || GetEnvironmentVariableA("PW_BENCH_DEBUG_LAYER", nullptr, 0) != 0) {
        ID3D12Debug *debug = nullptr;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))) && debug) { debug->EnableDebugLayer(); debug->Release(); std::printf("[info] D3D12 debug layer enabled\n"); }
    }
    WNDCLASSW wc{}; wc.lpfnWndProc = WndProc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"pw_bench";
    RegisterClassW(&wc);
    d.hwnd = o.fullscreenWindow
        ? CreateWindowW(L"pw_bench", L"PeripheralWarp bench", WS_POPUP | WS_VISIBLE, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN), nullptr, nullptr, wc.hInstance, nullptr)
        : CreateWindowW(L"pw_bench", L"PeripheralWarp bench", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 40, 40, 1600, 900, nullptr, nullptr, wc.hInstance, nullptr);

    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2; sd.BufferDesc.Width = o.presentW; sd.BufferDesc.Height = o.presentH; sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.OutputWindow = d.hwnd; sd.SampleDesc.Count = 1; sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_1;
    const UINT deviceFlags = o.debugLayer ? D3D11_CREATE_DEVICE_DEBUG : 0;
    if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, deviceFlags, &fl, 1, D3D11_SDK_VERSION, &sd, &d.sc, &d.dev, nullptr, &d.ctx))) {
        std::printf("[fail] D3D11 device\n"); return false;
    }
    std::printf("[info] D3D11 device created%s\n", o.debugLayer ? " (debug layer)" : "");
    if (ComPtr<ID3D11InfoQueue> q; o.debugLayer && SUCCEEDED(d.dev.As(&q))) { // keep only warnings and worse, no count limit
        D3D11_MESSAGE_SEVERITY deny[] = {D3D11_MESSAGE_SEVERITY_INFO, D3D11_MESSAGE_SEVERITY_MESSAGE};
        D3D11_INFO_QUEUE_FILTER filter{}; filter.DenyList.NumSeverities = 2; filter.DenyList.pSeverityList = deny;
        q->PushStorageFilter(&filter); q->SetMessageCountLimit(UINT64(-1));
    }

    // Render-resolution targets (or the reference's 4K targets), the DLSS output, the swap chain.
    const UINT sceneW = o.reference ? kOutW : kRenderW, sceneH = o.reference ? kOutH : kRenderH;
    auto tex = [&](UINT w, UINT h, DXGI_FORMAT f, UINT bind) {
        D3D11_TEXTURE2D_DESC td{}; td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1; td.Format = f; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = bind;
        ComPtr<ID3D11Texture2D> t; d.dev->CreateTexture2D(&td, nullptr, &t); return t; };
    d.color = tex(sceneW, sceneH, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
    d.motion = tex(sceneW, sceneH, DXGI_FORMAT_R16G16_FLOAT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
    d.depth = tex(sceneW, sceneH, DXGI_FORMAT_R24G8_TYPELESS, D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE);
    d.output = tex(kOutW, kOutH, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
    if (!d.color || !d.motion || !d.depth || !d.output) { std::printf("[fail] textures\n"); return false; }
    d.dev->CreateRenderTargetView(d.color.Get(), nullptr, &d.colorRtv);
    d.dev->CreateRenderTargetView(d.motion.Get(), nullptr, &d.motionRtv);
    d.dev->CreateRenderTargetView(d.output.Get(), nullptr, &d.outputRtv);
    D3D11_DEPTH_STENCIL_VIEW_DESC dd{}; dd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT; dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    d.dev->CreateDepthStencilView(d.depth.Get(), &dd, &d.dsv);
    d.dev->CreateShaderResourceView(d.output.Get(), nullptr, &d.outputSrv);
    d.dev->CreateShaderResourceView(d.color.Get(), nullptr, &d.colorSrv);
    d.sc->GetBuffer(0, IID_PPV_ARGS(&d.back));
    d.dev->CreateRenderTargetView(d.back.Get(), nullptr, &d.backRtv);
    return true;
}

void ReportD3D11Messages(const BenchOptions &o, BenchDevice &d)
{
    ComPtr<ID3D11InfoQueue> infoQueue;
    if (o.debugLayer && SUCCEEDED(d.dev.As(&infoQueue))) { // the D3D11 debug device's messages (the D3D12 side is logged by the add-on)
        UINT64 counts[5] = {};
        const UINT64 stored = infoQueue->GetNumStoredMessages();
        for (UINT64 m = 0; m < stored; ++m) {
            SIZE_T size = 0; infoQueue->GetMessage(m, nullptr, &size);
            std::vector<char> buffer(size); auto *msg = (D3D11_MESSAGE *) buffer.data();
            if (FAILED(infoQueue->GetMessage(m, msg, &size))) continue;
            ++counts[msg->Severity < 5 ? msg->Severity : 4];
            if (msg->Severity <= D3D11_MESSAGE_SEVERITY_WARNING) std::printf("[debug] d3d11 severity %d id %d: %.*s\n", (int) msg->Severity, (int) msg->ID, (int) msg->DescriptionByteLength, msg->pDescription);
        }
        std::printf("[debug] d3d11 messages: %llu corruption, %llu error, %llu warning, %llu info, %llu message (%llu discarded)\n", counts[0], counts[1], counts[2],
                    counts[3], counts[4], infoQueue->GetNumMessagesDiscardedByMessageCountLimit());
    } else if (o.debugLayer) std::printf("[debug] d3d11: no info queue (debug layer not installed?)\n");
}
