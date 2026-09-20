#include "device.h"

bool Device::Resize(unsigned w, unsigned h)
{
    if(!w || !h || (w==width && h==height)) return false;
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
