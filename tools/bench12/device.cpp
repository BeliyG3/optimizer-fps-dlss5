#include "device.h"
#include <d3d12sdklayers.h>
#include <cstring>

namespace {
LRESULT CALLBACK WindowProc(HWND window, UINT msg, WPARAM w, LPARAM l)
{
    auto *device=reinterpret_cast<Device *>(GetWindowLongPtrW(window,GWLP_USERDATA));
    if(device && device->messageHandler && device->messageHandler(window,msg,w,l)) return 1;
    if(msg==WM_CLOSE) { PostQuitMessage(0); return 0; }
    if(msg==WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(window,msg,w,l);
}
}
std::filesystem::path ExecutableDirectory()
{
    wchar_t path[32768]; DWORD n=GetModuleFileNameW(nullptr,path,32768);
    if(!n || n==32768) throw std::runtime_error("Cannot locate executable directory");
    return std::filesystem::path(path).parent_path();
}
void Transition(ID3D12GraphicsCommandList *list, ID3D12Resource *r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER b{}; b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,before,after}; list->ResourceBarrier(1,&b);
}
void UavBarrier(ID3D12GraphicsCommandList *list, ID3D12Resource *r)
{
    D3D12_RESOURCE_BARRIER b{}; b.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV; b.UAV.pResource=r; list->ResourceBarrier(1,&b);
}
Device::Device(unsigned w, unsigned h, bool debug, bool vsync, bool interactive):width(w),height(h),debugEnabled(debug),vsyncEnabled(vsync)
{
    if(debug) {
        ComPtr<ID3D12Debug> layer;
        Check(D3D12GetDebugInterface(IID_PPV_ARGS(&layer)),"D3D12 debug layer (install Windows Graphics Tools)");
        layer->EnableDebugLayer();
    }
    ComPtr<IDXGIFactory6> factory;
    Check(CreateDXGIFactory2(debug ? DXGI_CREATE_FACTORY_DEBUG : 0,IID_PPV_ARGS(&factory)),"CreateDXGIFactory2");
    BOOL tearing=FALSE;
    tearingSupported=SUCCEEDED(factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING,&tearing,sizeof(tearing))) && tearing;
    for(UINT i=0;;++i) {
        ComPtr<IDXGIAdapter1> adapter;
        HRESULT hr=factory->EnumAdapterByGpuPreference(i,DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,IID_PPV_ARGS(&adapter));
        if(hr==DXGI_ERROR_NOT_FOUND) break;
        Check(hr,"EnumAdapterByGpuPreference");
        DXGI_ADAPTER_DESC1 desc{}; Check(adapter->GetDesc1(&desc),"GetDesc1");
        if(desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        ComPtr<ID3D12Device5> candidate;
        if(FAILED(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_12_1,IID_PPV_ARGS(&candidate)))) continue;
        D3D12_FEATURE_DATA_D3D12_OPTIONS5 feature{};
        if(FAILED(candidate->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5,&feature,sizeof(feature))) || feature.RaytracingTier<D3D12_RAYTRACING_TIER_1_1) continue;
        D3D12_FEATURE_DATA_SHADER_MODEL model{D3D_SHADER_MODEL_6_5};
        if(FAILED(candidate->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL,&model,sizeof(model))) || model.HighestShaderModel<D3D_SHADER_MODEL_6_5) continue;
        gpu=candidate; std::printf("[info] adapter: %ls\n",desc.Description); break;
    }
    if(!gpu) throw std::runtime_error("Requires a hardware D3D12 feature-level 12_1 device with DXR tier 1.1 and shader model 6.5");
    D3D12_COMMAND_QUEUE_DESC q{}; q.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
    Check(gpu->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)),"CreateCommandQueue");
    WNDCLASSW wc{}; wc.lpfnWndProc=WindowProc; wc.hInstance=GetModuleHandleW(nullptr); wc.lpszClassName=L"pw_bench12";
    if(!RegisterClassW(&wc) && GetLastError()!=ERROR_CLASS_ALREADY_EXISTS) throw std::runtime_error("RegisterClass failed");
    const DWORD style=interactive ? WS_OVERLAPPEDWINDOW : WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX;
    RECT rect{0,0,LONG(w),LONG(h)}; AdjustWindowRect(&rect,style,FALSE);
    window=CreateWindowW(wc.lpszClassName,L"pw_bench12",style,CW_USEDEFAULT,CW_USEDEFAULT,rect.right-rect.left,rect.bottom-rect.top,nullptr,nullptr,wc.hInstance,nullptr);
    if(!window) throw std::runtime_error("CreateWindow failed");
    SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(this));
    DXGI_SWAP_CHAIN_DESC1 sd{}; sd.Width=w; sd.Height=h; sd.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count=1; sd.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.BufferCount=2; sd.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.Flags=tearingSupported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    ComPtr<IDXGISwapChain1> initial;
    Check(factory->CreateSwapChainForHwnd(queue.Get(),window,&sd,nullptr,nullptr,&initial),"CreateSwapChainForHwnd");
    Check(initial.As(&swap),"Query swap chain"); Check(factory->MakeWindowAssociation(window,DXGI_MWA_NO_ALT_ENTER),"MakeWindowAssociation");
    D3D12_DESCRIPTOR_HEAP_DESC hd{}; hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors=2;
    Check(gpu->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&rtvHeap)),"Create RTV heap");
    rtvStep=gpu->GetDescriptorHandleIncrementSize(hd.Type);
    auto rtv=rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for(UINT i=0;i<2;++i) { Check(swap->GetBuffer(i,IID_PPV_ARGS(&back[i])),"Get back buffer"); gpu->CreateRenderTargetView(back[i].Get(),nullptr,rtv); rtv.ptr+=rtvStep; }
    hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors=65536; hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    Check(gpu->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap)),"Create shader heap"); descriptorStep=gpu->GetDescriptorHandleIncrementSize(hd.Type);
    Check(gpu->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)),"CreateCommandAllocator");
    Check(gpu->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list)),"CreateCommandList");
    Check(list->Close(),"Close initial list"); Check(gpu->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"CreateFence");
    event=CreateEventW(nullptr,FALSE,FALSE,nullptr); if(!event) throw std::runtime_error("CreateEvent failed");
    ShowWindow(window,SW_SHOW); PrintMessages();
}
Device::~Device()
{
    // Every submission is retired synchronously, including uploads and frame dumps.
    if(event) CloseHandle(event);
    if(window && IsWindow(window)) DestroyWindow(window);
}
void Device::Begin() { Check(allocator->Reset(),"Reset allocator"); Check(list->Reset(allocator.Get(),nullptr),"Reset list"); }
void Device::Wait()
{
    Check(queue->Signal(fence.Get(),++serial),"Signal fence");
    if(fence->GetCompletedValue()<serial) {
        Check(fence->SetEventOnCompletion(serial,event),"SetEventOnCompletion");
        if(WaitForSingleObject(event,INFINITE)!=WAIT_OBJECT_0) throw std::runtime_error("Fence wait failed");
    }
    Check(gpu->GetDeviceRemovedReason(),"Device health");
}
void Device::Submit(bool present)
{
    Check(list->Close(),"Close list"); ID3D12CommandList *lists[]={list.Get()}; queue->ExecuteCommandLists(1,lists);
    if(onSubmitted) onSubmitted(submissionContext,queue.Get(),list.Get());
    // Retire even if Present fails, so transient resources cannot be released while in flight.
    HRESULT hr=present ? swap->Present(vsyncEnabled ? 1u : 0u,!vsyncEnabled && tearingSupported ? DXGI_PRESENT_ALLOW_TEARING : 0u) : S_OK;
    Wait(); PrintMessages(); Check(hr,"Present");
}
bool Device::Pump()
{
    MSG msg{}; while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)) { if(msg.message==WM_QUIT) return false; TranslateMessage(&msg); DispatchMessageW(&msg); } return true;
}
unsigned Device::Allocate(unsigned count)
{
    if(count>65536-nextDescriptor) throw std::runtime_error("Shader descriptor heap exhausted");
    unsigned first=nextDescriptor; nextDescriptor+=count; return first;
}
D3D12_CPU_DESCRIPTOR_HANDLE Device::Cpu(unsigned i) const { auto h=heap->GetCPUDescriptorHandleForHeapStart(); h.ptr+=SIZE_T(i)*descriptorStep; return h; }
D3D12_GPU_DESCRIPTOR_HANDLE Device::Gpu(unsigned i) const { auto h=heap->GetGPUDescriptorHandleForHeapStart(); h.ptr+=UINT64(i)*descriptorStep; return h; }
ID3D12Resource *Device::BackBuffer() const { return back[swap->GetCurrentBackBufferIndex()].Get(); }
D3D12_CPU_DESCRIPTOR_HANDLE Device::BackRtv() const { auto h=rtvHeap->GetCPUDescriptorHandleForHeapStart(); h.ptr+=SIZE_T(swap->GetCurrentBackBufferIndex())*rtvStep; return h; }
ComPtr<ID3D12Resource> Device::Buffer(UINT64 bytes, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state, D3D12_RESOURCE_FLAGS flags)
{
    D3D12_HEAP_PROPERTIES hp{}; hp.Type=type;
    D3D12_RESOURCE_DESC d{}; d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER; d.Width=bytes; d.Height=1; d.DepthOrArraySize=1;
    d.MipLevels=1; d.SampleDesc.Count=1; d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR; d.Flags=flags;
    ComPtr<ID3D12Resource> r; Check(gpu->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,state,nullptr,IID_PPV_ARGS(&r)),"Create buffer"); return r;
}
ComPtr<ID3D12Resource> Device::UploadBuffer(const void *data, UINT64 bytes)
{
    auto upload=Buffer(bytes,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
    void *p=nullptr; D3D12_RANGE empty{0,0}; Check(upload->Map(0,&empty,&p),"Map upload"); std::memcpy(p,data,size_t(bytes)); upload->Unmap(0,nullptr);
    auto result=Buffer(bytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COPY_DEST);
    Begin(); list->CopyBufferRegion(result.Get(),0,upload.Get(),0,bytes);
    Transition(list.Get(),result.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE); Submit(); return result;
}
ComPtr<ID3D12Resource> Device::Texture(unsigned w, unsigned h, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state, unsigned mips)
{
    D3D12_HEAP_PROPERTIES hp{}; hp.Type=D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d{}; d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width=w; d.Height=h; d.DepthOrArraySize=1;
    d.MipLevels=UINT16(mips); d.Format=format; d.SampleDesc.Count=1; d.Flags=flags;
    ComPtr<ID3D12Resource> r; Check(gpu->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,state,nullptr,IID_PPV_ARGS(&r)),"Create texture"); return r;
}
void Device::TextureSrv(ID3D12Resource *r, unsigned descriptor)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC s{}; s.Format=r->GetDesc().Format; s.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
    s.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; s.Texture2D.MipLevels=r->GetDesc().MipLevels; gpu->CreateShaderResourceView(r,&s,Cpu(descriptor));
}
void Device::BufferSrv(ID3D12Resource *r, unsigned descriptor, unsigned count, unsigned stride)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC s{}; s.ViewDimension=D3D12_SRV_DIMENSION_BUFFER; s.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    s.Buffer.NumElements=count; s.Buffer.StructureByteStride=stride; gpu->CreateShaderResourceView(r,&s,Cpu(descriptor));
}
void Device::PrintMessages()
{
    if(!debugEnabled) return;
    ComPtr<ID3D12InfoQueue> info; Check(gpu.As(&info),"Query debug info queue"); bool errors=false;
    for(UINT64 i=0;i<info->GetNumStoredMessages();++i) {
        SIZE_T bytes=0; Check(info->GetMessage(i,nullptr,&bytes),"Get debug message size"); std::vector<BYTE> data(bytes);
        auto *m=reinterpret_cast<D3D12_MESSAGE *>(data.data()); Check(info->GetMessage(i,m,&bytes),"Get debug message");
        std::fprintf(stderr,"[d3d12:%d] %s\n",int(m->Severity),m->pDescription);
        errors|=m->Severity<=D3D12_MESSAGE_SEVERITY_ERROR;
    }
    info->ClearStoredMessages(); if(errors) throw std::runtime_error("D3D12 debug layer reported errors");
}
