#include "device.h"

bool Device::Resize(unsigned w, unsigned h, bool force)
{
    if(!w || !h || (!force && w==width && h==height)) return false;
    Wait();
    for(auto &buffer:back) buffer.Reset();
    readback.Reset();
    Check(swap->ResizeBuffers(2,w,h,DXGI_FORMAT_R8G8B8A8_UNORM,
        tearingSupported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0),"Resize swap chain");
    width=w; height=h;
    auto rtv=rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for(unsigned i=0;i<2;++i) {
        Check(swap->GetBuffer(i,IID_PPV_ARGS(&back[i])),"Get resized back buffer");
        gpu->CreateRenderTargetView(back[i].Get(),nullptr,rtv); rtv.ptr+=rtvStep;
    }
    return true;
}

void Device::RecreateSwapChain()
{
    Wait();
    DXGI_SWAP_CHAIN_DESC1 sd{}; Check(swap->GetDesc1(&sd),"Swap chain description");
    ComPtr<IDXGIFactory2> factory; Check(swap->GetParent(IID_PPV_ARGS(&factory)),"Swap chain factory");
    for(auto &buffer:back) buffer.Reset();
    readback.Reset();
    swap.Reset(); // the last reference: the swap chain is destroyed before the new one exists
    ComPtr<IDXGISwapChain1> created;
    Check(factory->CreateSwapChainForHwnd(queue.Get(),window,&sd,nullptr,nullptr,&created),"Recreate swap chain");
    Check(created.As(&swap),"Query recreated swap chain");
    Check(factory->MakeWindowAssociation(window,DXGI_MWA_NO_ALT_ENTER),"MakeWindowAssociation");
    auto rtv=rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for(unsigned i=0;i<2;++i) {
        Check(swap->GetBuffer(i,IID_PPV_ARGS(&back[i])),"Get recreated back buffer");
        gpu->CreateRenderTargetView(back[i].Get(),nullptr,rtv); rtv.ptr+=rtvStep;
    }
}
