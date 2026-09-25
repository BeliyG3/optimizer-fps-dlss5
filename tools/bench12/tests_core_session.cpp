#include "core_session.h"
#include <cstdio>

// These lifecycle tests never create a model or load a vendor runtime.
NVSDK_NGX_Result NrRuntime::Create(ID3D12GraphicsCommandList *, NVSDK_NGX_Parameter *, NVSDK_NGX_Handle **) const {
    return NVSDK_NGX_Result_FAIL_InvalidParameter;
}
NVSDK_NGX_Result NrRuntime::Evaluate(ID3D12GraphicsCommandList *, NVSDK_NGX_Handle *, NVSDK_NGX_Parameter *) const {
    return NVSDK_NGX_Result_FAIL_InvalidParameter;
}
NVSDK_NGX_Result NrRuntime::Release(NVSDK_NGX_Handle *) const {
    return NVSDK_NGX_Result_FAIL_InvalidParameter;
}

namespace {
void Require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}
HANDLE DirectEvent() {
    wchar_t name[96];
    swprintf_s(name, L"Local\\OptimizerFpsDirectHost_%lu", GetCurrentProcessId());
    return OpenEventW(SYNCHRONIZE, FALSE, name);
}
}
int main(int argc, char **argv) {
    try {
        Require(argc == 2, "expected core DLL directory");
        ComPtr<IDXGIFactory4> factory;
        Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "factory");
        ComPtr<IDXGIAdapter> adapter;
        Check(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)), "WARP");
        ComPtr<ID3D12Device> device;
        Check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "device");
        D3D12_COMMAND_QUEUE_DESC desc{};
        ComPtr<ID3D12CommandQueue> queue;
        Check(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue)), "queue");
        NrRuntime runtime;
        NrCreateInfo create;
        NrFrame frame;
        CoreSession session(runtime, create, frame);
        const auto host = std::filesystem::absolute(argv[1]) / L"pw_bench12.exe";
        bool rejected = false;
        try { session.Open(host / L"missing" / L"pw_bench12.exe", device.Get(), queue.Get(), 2, 0, 0); }
        catch (const std::runtime_error &) { rejected = true; }
        Require(rejected, "missing core must fail");
        Require(DirectEvent() == nullptr, "failed Open leaked event");
        Require(session.Close(), "Close after failed Open");
        for (int cycle = 0; cycle < 2; ++cycle) {
            session.Open(host, device.Get(), queue.Get(), 2, 0, 0);
            HANDLE event = DirectEvent();
            Require(event && WaitForSingleObject(event, 0) == WAIT_OBJECT_0, "event not signaled");
            session.Open(host, device.Get(), queue.Get(), 2, 0, 0);
            Require(WaitForSingleObject(event, 0) == WAIT_OBJECT_0, "repeated Open reset event");
            session.Open(host, device.Get(), queue.Get(), 0, 0, 2);
            Require(session.Close() && session.Close(), "repeated Close");
            Require(WaitForSingleObject(event, 0) == WAIT_TIMEOUT, "Close did not reset event");
            CloseHandle(event);
            Require(DirectEvent() == nullptr, "Close leaked event");
        }
        std::puts("[pass] core session repeat Open/Close and failed Open");
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "[fail] %s\n", error.what());
        return 1;
    }
}
