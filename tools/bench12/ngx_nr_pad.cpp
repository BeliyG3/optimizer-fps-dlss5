#include "ngx_nr_pad.h"
#include <cmath>

namespace {
constexpr float Magenta[4]={1,0,1,1};
constexpr unsigned short HalfOne=0x3C00;
bool IsMagenta(const unsigned short *texel) { return texel[0]==HalfOne && texel[1]==0 && texel[2]==HalfOne && texel[3]==HalfOne; }
double HalfToFloat(unsigned short h)
{
    const int exponent=(h>>10)&0x1f, mantissa=h&0x3ff; const double sign=(h&0x8000) ? -1.0 : 1.0;
    if(exponent==0) return sign*std::ldexp(mantissa,-24);
    if(exponent==31) return mantissa ? 0.0 : sign*65504.0; // NaN reads as 0, infinity as the largest half
    return sign*std::ldexp(mantissa+1024,exponent-25);
}
constexpr auto Uav=D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
constexpr auto Source=D3D12_RESOURCE_STATE_COPY_SOURCE;
constexpr auto Dest=D3D12_RESOURCE_STATE_COPY_DEST;
}
void NrOutputPad::Configure(Device &d, ID3D12Resource *target, unsigned w, unsigned h, const NrPadLayout &l)
{
    Reset();
    if(!l.padX && !l.padY) return;
    output=target; frameWidth=w; frameHeight=h; pad=l;
    const auto desc=output->GetDesc(); UINT64 bytes=0;
    d.gpu->GetCopyableFootprints(&desc,0,1,0,&layout,nullptr,nullptr,&bytes);
    readback=d.Buffer(bytes,D3D12_HEAP_TYPE_READBACK,Dest);
    if(!haveDescriptor) {
        // ClearUnorderedAccessViewFloat needs the view twice: on the bound heap and on a CPU-only heap.
        descriptor=d.Allocate(1); haveDescriptor=true;
        D3D12_DESCRIPTOR_HEAP_DESC heap{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,1,D3D12_DESCRIPTOR_HEAP_FLAG_NONE,0};
        Check(d.gpu->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&cpuHeap)),"Create NR pad clear heap");
    }
    D3D12_UNORDERED_ACCESS_VIEW_DESC view{}; view.Format=desc.Format; view.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
    d.gpu->CreateUnorderedAccessView(output,nullptr,&view,d.Cpu(descriptor));
    d.gpu->CreateUnorderedAccessView(output,nullptr,&view,cpuHeap->GetCPUDescriptorHandleForHeapStart());
    std::printf("[nr] output texture %llux%u, region %ux%u at %u,%u; the pad is filled with magenta before each evaluate\n",
        desc.Width,desc.Height,w,h,l.baseX,l.baseY);
}
void NrOutputPad::Fill(Device &d)
{
    const auto desc=output->GetDesc();
    const LONG width=LONG(desc.Width), height=LONG(desc.Height);
    const LONG x0=LONG(pad.baseX), y0=LONG(pad.baseY), x1=x0+LONG(frameWidth), y1=y0+LONG(frameHeight);
    D3D12_RECT rects[4]; UINT count=0;
    auto add=[&](LONG left, LONG top, LONG right, LONG bottom) { if(right>left && bottom>top) rects[count++]={left,top,right,bottom}; };
    add(0,0,width,y0); add(0,y1,width,height); add(0,y0,x0,y1); add(x1,y0,width,y1);
    if(!count) return; // zero rects would clear the whole view
    d.list->ClearUnorderedAccessViewFloat(d.Gpu(descriptor),cpuHeap->GetCPUDescriptorHandleForHeapStart(),output,Magenta,count,rects);
    UavBarrier(d.list.Get(),output);
}
void NrOutputPad::Capture(Device &d, int frame)
{
    auto *list=d.list.Get();
    Transition(list,output,Uav,Source);
    D3D12_TEXTURE_COPY_LOCATION src{}, all{};
    src.pResource=output; src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    all.pResource=readback.Get(); all.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; all.PlacedFootprint=layout;
    list->CopyTextureRegion(&all,0,0,0,&src,nullptr);
    Transition(list,output,Source,Uav);
    checkPending=true; checkFrame=frame;
}
void NrOutputPad::Report()
{
    if(!checkPending) return;
    checkPending=false;
    void *mapped=nullptr; Check(readback->Map(0,nullptr,&mapped),"Map NR pad readback");
    const auto *bytes=static_cast<const unsigned char *>(mapped);
    const unsigned x0=pad.baseX, y0=pad.baseY, x1=x0+frameWidth, y1=y0+frameHeight;
    const unsigned width=layout.Footprint.Width, height=layout.Footprint.Height;
    unsigned total=0, top=0, bottom=0, left=0, right=0;
    double sum[3]{};
    for(unsigned y=0;y<height;++y) {
        const auto *row=reinterpret_cast<const unsigned short *>(bytes+layout.Offset+size_t(y)*layout.Footprint.RowPitch);
        for(unsigned x=0;x<width;++x) {
            if(x>=x0 && x<x1 && y>=y0 && y<y1) continue;
            ++total;
            const auto *texel=row+size_t(x)*4;
            if(IsMagenta(texel)) continue;
            if(y<y0) ++top; else if(y>=y1) ++bottom; else if(x<x0) ++left; else ++right;
            for(int c=0;c<3;++c) sum[c]+=HalfToFloat(texel[c]);
        }
    }
    D3D12_RANGE none{0,0}; readback->Unmap(0,&none);
    const unsigned changed=top+bottom+left+right;
    if(!changed) std::printf("[nr] frame %d pad check: 0 of %u pad texels changed; the pad is untouched\n",checkFrame,total);
    else std::printf("[nr] frame %d pad check: %u of %u pad texels changed (top %u, bottom %u, left %u, right %u), their mean RGB %.3f %.3f %.3f\n",
        checkFrame,changed,total,top,bottom,left,right,sum[0]/changed,sum[1]/changed,sum[2]/changed);
}
