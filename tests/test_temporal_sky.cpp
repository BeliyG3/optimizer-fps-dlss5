// TemporalSkyField through the temporal machine on a D3D12 WARP device (test_temporal_sky_machine.cpp: the synchronous
// routes; test_temporal_sky_background.cpp: the background mode's adoption); the debug
// layer, when installed, must report no error about the field's states and bindings.
// Usage: ofps_temporal_sky_tests <shader dir> <master fixture dir> <round 1 fixture dir> <shared-rect pass dir>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <iostream>
#include <vector>

int TestTemporalSkyMachine(ID3D12Device *device, ID3D12CommandQueue *queue, const char *shaderDirectory, const char *masterFixture,
                           const char *round1Fixture);
int TestTemporalSkyBackground(ID3D12Device *device, ID3D12CommandQueue *queue, const char *shaderDirectory, const char *masterFixture,
                              const char *sharedRectDirectory);

int main(int argc, char **argv)
{
    if (argc != 5) {
        std::cerr << "usage: ofps_temporal_sky_tests <shader dir> <master fixture dir> <round 1 fixture dir> <shared-rect pass dir>\n";
        return 1;
    }
    using Microsoft::WRL::ComPtr;
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter> warp;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    D3D12_COMMAND_QUEUE_DESC desc{};
    desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || FAILED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))) ||
        FAILED(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))) ||
        FAILED(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue)))) {
        std::cerr << "FAIL: a D3D12 WARP device and queue\n";
        return 1;
    }
    int failures = TestTemporalSkyMachine(device.Get(), queue.Get(), argv[1], argv[2], argv[3]);
    failures += TestTemporalSkyBackground(device.Get(), queue.Get(), argv[1], argv[2], argv[4]);
    ComPtr<ID3D12InfoQueue> info;
    if (debug && SUCCEEDED(device.As(&info))) {
        for (UINT64 i = 0, n = info->GetNumStoredMessages(); i < n; ++i) {
            SIZE_T bytes = 0;
            if (FAILED(info->GetMessage(i, nullptr, &bytes))) continue;
            std::vector<char> storage(bytes);
            auto *message = reinterpret_cast<D3D12_MESSAGE *>(storage.data());
            if (SUCCEEDED(info->GetMessage(i, message, &bytes)) && message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
                std::cerr << "FAIL: D3D12 debug layer: " << message->pDescription << '\n';
                ++failures;
            }
        }
    }
    if (failures == 0) std::cout << "PASS: temporal sky field through the machine\n";
    return failures == 0 ? 0 : 1;
}
