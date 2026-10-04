// The carried-frame grid: the pure rule (CarriedGridFor), its use per feature (test_temporal_grid_frame.cpp) and,
// with a shader directory, the machine's route on a D3D12 WARP device (test_temporal_grid_machine.cpp) and its
// changes of grid size on one machine (test_temporal_grid_switch.cpp).
// Usage: ofps_temporal_grid_tests [<shader dir>]
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "core/temporal/grid.h"

#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <iostream>
#include <limits>
#include <vector>

using ofps::core::temporal::CarriedGrid;
using ofps::core::temporal::CarriedGridFor;
using ofps::sdk::LayoutV2;
using ofps::sdk::WarpMode;

int TestTemporalGridMachine(ID3D12Device *device, ID3D12CommandQueue *queue, const char *shaderDirectory); // test_temporal_grid_machine.cpp
int TestTemporalGridSwitch(ID3D12Device *device, ID3D12CommandQueue *queue, const char *shaderDirectory); // test_temporal_grid_switch.cpp
int TestCarriedGridFrame(); // test_temporal_grid_frame.cpp

namespace {
int failures = 0;

void Check(bool condition, const char *message)
{
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
}

LayoutV2 Layout(WarpMode mode, float workX, float workY, float global, std::uint32_t width, std::uint32_t height)
{
    ofps::sdk::ConfigV2 config = ofps::sdk::DefaultConfigV2();
    config.mode = mode;
    config.xAxis = {mode == WarpMode::Peripheral ? 80.0f : workX, workX};
    config.yAxis = {mode == WarpMode::Peripheral ? 80.0f : workY, workY};
    config.globalScalePercent = global;
    LayoutV2 layout{};
    Check(ofps::sdk::BuildLayout(config, width, height, &layout) == ofps::sdk::Status::Ok, "grid rule: the test layout builds");
    return layout;
}

bool Is(CarriedGrid grid, std::uint32_t width, std::uint32_t height) { return grid.width == width && grid.height == height; }
bool Native(CarriedGrid grid) { return Is(grid, 0, 0); }

void TestCarriedGridFor()
{
    // Peripheral: the centre's density, work / raw work; 3456x1944 raw work at 4K.
    const LayoutV2 p50 = Layout(WarpMode::Peripheral, 90, 90, 50, 3840, 2160);
    Check(Is(CarriedGridFor(p50, true, true), 1920, 1080), "Peripheral 90/90 at GlobalScale 50 carries on 1920x1080");
    Check(Is(CarriedGridFor(Layout(WarpMode::Peripheral, 90, 90, 40, 3840, 2160), true, true), 1538, 865),
          "Peripheral at GlobalScale 40: ceil(native * work / raw work) from the integers");
    Check(Native(CarriedGridFor(Layout(WarpMode::Peripheral, 90, 90, 80, 3840, 2160), true, true)),
          "Peripheral at GlobalScale 80: the quantized density 0.8003 is above the gate");
    Check(Native(CarriedGridFor(Layout(WarpMode::Peripheral, 90, 90, 100, 3840, 2160), true, true)), "Peripheral at GlobalScale 100 stays native");
    // Uniform: work / native; the grid is the model's own size.
    Check(Is(CarriedGridFor(Layout(WarpMode::Uniform, 80, 90, 50, 3840, 2160), true, true), 1536, 972), "Uniform 80/90 at GlobalScale 50");
    Check(Is(CarriedGridFor(Layout(WarpMode::Uniform, 80, 90, 80, 3840, 2160), true, true), 2458, 1556), "Uniform 80/90 at GlobalScale 80");
    Check(Native(CarriedGridFor(Layout(WarpMode::Uniform, 80, 90, 100, 3840, 2160), true, true)), "Uniform 80/90 at 100: Y density 0.9 gates");
    Check(Is(CarriedGridFor(Layout(WarpMode::Uniform, 80, 80, 100, 3840, 2160), true, true), 3072, 1728), "a density of exactly 0.8 passes");
    Check(Native(CarriedGridFor(Layout(WarpMode::Off, 90, 90, 100, 3840, 2160), true, true)), "Mode Off stays native");
    // The caller's switches.
    Check(Native(CarriedGridFor(p50, false, true)), "an unwarped frame stays native");
    Check(Native(CarriedGridFor(p50, true, false)), "the grid switched off stays native");
    // Odd sizes: ceil, never below the model's density.
    Check(Is(CarriedGridFor(Layout(WarpMode::Peripheral, 90, 90, 50, 1001, 777), true, true), 502, 389), "Peripheral at 1001x777");
    Check(Is(CarriedGridFor(Layout(WarpMode::Peripheral, 90, 90, 50, 1919, 1081), true, true), 960, 542), "Peripheral at 1919x1081");
    Check(Is(CarriedGridFor(Layout(WarpMode::Uniform, 70, 60, 50, 1001, 777), true, true), 352, 234), "Uniform at 1001x777");
    // Small frames: no grid under 32 texels a side; never below 16 texels.
    Check(Native(CarriedGridFor(Layout(WarpMode::Uniform, 50, 50, 100, 31, 600), true, true)), "a 31-texel side stays native");
    Check(Is(CarriedGridFor(Layout(WarpMode::Uniform, 50, 50, 100, 32, 32), true, true), 16, 16), "32x32 at density 0.5");
    Check(Is(CarriedGridFor(Layout(WarpMode::Uniform, 25, 25, 100, 40, 40), true, true), 16, 16), "the grid never goes below 16 texels");
    LayoutV2 tiny = p50;
    tiny.nativeWidth = tiny.nativeHeight = 2;
    Check(Native(CarriedGridFor(tiny, true, true)), "a 2x2 frame stays native");
    // Densities that are not usable.
    for (const float bad : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(), 0.0f, -0.5f}) {
        LayoutV2 broken = p50;
        broken.effectiveScaleY = bad;
        Check(Native(CarriedGridFor(broken, true, true)), "a non-finite or non-positive density stays native");
    }
    LayoutV2 noRaw = p50;
    noRaw.rawWorkWidth = 0;
    Check(Native(CarriedGridFor(noRaw, true, true)), "a layout without raw work stays native");
    LayoutV2 unknown = p50;
    unknown.mode = static_cast<WarpMode>(7);
    Check(Native(CarriedGridFor(unknown, true, true)), "an unknown mode stays native");
}

int RunMachineTests(const char *shaderDirectory)
{
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
    int result = TestTemporalGridMachine(device.Get(), queue.Get(), shaderDirectory);
    result += TestTemporalGridSwitch(device.Get(), queue.Get(), shaderDirectory);
    // The debug layer, when installed, must have nothing to say about the grid route's states and bindings.
    ComPtr<ID3D12InfoQueue> info;
    if (debug && SUCCEEDED(device.As(&info))) {
        for (UINT64 i = 0, n = info->GetNumStoredMessages(); i < n; ++i) {
            SIZE_T bytes = 0;
            if (FAILED(info->GetMessage(i, nullptr, &bytes))) continue;
            std::vector<char> storage(bytes);
            auto *message = reinterpret_cast<D3D12_MESSAGE *>(storage.data());
            if (SUCCEEDED(info->GetMessage(i, message, &bytes)) && message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
                std::cerr << "FAIL: D3D12 debug layer: " << message->pDescription << '\n';
                ++result;
            }
        }
    }
    return result;
}
} // namespace

int main(int argc, char **argv)
{
    TestCarriedGridFor();
    failures += TestCarriedGridFrame();
    if (argc > 1) failures += RunMachineTests(argv[1]);
    if (failures == 0) std::cout << "PASS: temporal grid\n";
    return failures == 0 ? 0 : 1;
}
