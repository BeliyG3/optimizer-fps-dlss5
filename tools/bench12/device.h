#pragma once
#include "image_mips.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <vector>
#include <string>
#include <stdexcept>
#include <cstdio>
#include <filesystem>

using Microsoft::WRL::ComPtr;
inline void Check(HRESULT hr, const char *operation)
{
    if (FAILED(hr)) { char s[256]; std::snprintf(s,sizeof(s),"%s failed (HRESULT 0x%08lX)",operation,(unsigned long)hr); throw std::runtime_error(s); }
}
std::filesystem::path ExecutableDirectory();
void Transition(ID3D12GraphicsCommandList *list, ID3D12Resource *r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after);
void UavBarrier(ID3D12GraphicsCommandList *list, ID3D12Resource *r);

class Device {
    // Declared first, destroyed last: driver hooks may remain in D3D12 COM objects
    // after NGX shutdown. Keep their code loaded through the final COM Release.
    struct DriverModules {
        std::vector<HMODULE> handles;
        ~DriverModules() { for(auto handle:handles) FreeLibrary(handle); }
    } driverModules;
public:
    Device(unsigned width, unsigned height, bool debug, bool vsync=false, bool interactive=false);
    ~Device();
    Device(const Device &)=delete;
    Device &operator=(const Device &)=delete;
    ComPtr<ID3D12Device5> gpu;
    ComPtr<ID3D12GraphicsCommandList4> list;
    ComPtr<ID3D12DescriptorHeap> heap;
    unsigned width, height;
    void Begin();
    void Submit(bool present=false);
    void Wait();
    void RetainDriverModule(HMODULE module) { driverModules.handles.push_back(module); }
    bool Pump();
    HWND Window() const { return window; }
    ID3D12CommandQueue *Queue() const { return queue.Get(); }
    void SetVsync(bool enabled) { vsyncEnabled=enabled; }
    bool Resize(unsigned w, unsigned h);
    LRESULT (*messageHandler)(HWND,UINT,WPARAM,LPARAM)=nullptr;
    double lastFrameMs=0, lastTraceMs=0, lastUpscalerMs=0;
    double lastBlasMs=0;
    double wallFrameMs=0;
    unsigned Allocate(unsigned count=1);
    D3D12_CPU_DESCRIPTOR_HANDLE Cpu(unsigned index) const;
    D3D12_GPU_DESCRIPTOR_HANDLE Gpu(unsigned index) const;
    D3D12_CPU_DESCRIPTOR_HANDLE BackRtv() const;
    ID3D12Resource *BackBuffer() const;
    ComPtr<ID3D12Resource> Buffer(UINT64 bytes, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state,
                                 D3D12_RESOURCE_FLAGS flags=D3D12_RESOURCE_FLAG_NONE);
    ComPtr<ID3D12Resource> UploadBuffer(const void *data, UINT64 bytes);
    ComPtr<ID3D12Resource> Texture(unsigned w, unsigned h, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags,
                                  D3D12_RESOURCE_STATES state, unsigned mips=1);
    ComPtr<ID3D12Resource> UploadTexture(unsigned w, unsigned h, DXGI_FORMAT format, unsigned pixelBytes, const void *data);
    ComPtr<ID3D12Resource> UploadMips(DXGI_FORMAT format, unsigned pixelBytes, const std::vector<ImageMip> &levels);
    void TextureSrv(ID3D12Resource *r, unsigned descriptor);
    void BufferSrv(ID3D12Resource *r, unsigned descriptor, unsigned count, unsigned stride);
    void QueueDump();
    void WriteDump(int frame);
    void PrintMessages();
    void Timestamp(unsigned index);
    void ResolveTimings();
    void CollectTimings();
    void ReportTimings() const;
private:
    HWND window=nullptr;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<IDXGISwapChain3> swap;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    ComPtr<ID3D12Resource> back[2], readback;
    ComPtr<ID3D12Fence> fence;
    HANDLE event=nullptr;
    UINT64 serial=0;
    unsigned nextDescriptor=0, descriptorStep=0, rtvStep=0;
    bool debugEnabled=false;
    bool vsyncEnabled=false, tearingSupported=false;
    ComPtr<ID3D12QueryHeap> timestamps;
    ComPtr<ID3D12Resource> timingReadback;
    UINT64 timestampFrequency=0, timedFrames=0;
    double frameMilliseconds=0, traceMilliseconds=0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT dumpLayout{};
};
