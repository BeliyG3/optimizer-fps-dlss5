#pragma once
#include "device.h"

struct NrPadLayout { unsigned padX=0, padY=0, baseX=0, baseY=0; };

// --nr-output-pad X,Y[,BX,BY]: DLSSNR.Output is X/Y texels larger than the frame and the host's
// OutputSubrect (frame size) sits at BX,BY. Before every evaluate the pad is filled with a control
// colour (magenta 1,0,1,1) so that writes outside the region show up. Only the region is presented
// (NrColourBridge copies it out); on report frames the whole texture is read back and the pad is
// checked once the frame has been submitted.
class NrOutputPad {
public:
    void Configure(Device &device, ID3D12Resource *output, unsigned frameWidth, unsigned frameHeight, const NrPadLayout &layout);
    void Reset() { readback.Reset(); output=nullptr; checkPending=false; }
    bool Enabled() const { return output!=nullptr; }
    // The output must be a UAV (on the bench's shader-visible heap).
    void Fill(Device &device);
    // Output a UAV before and after: queues a readback of the whole texture for Report.
    void Capture(Device &device, int frame);
    // Call once the frame that queued a capture has been submitted and retired.
    void Report();
private:
    ID3D12Resource *output=nullptr;
    ComPtr<ID3D12Resource> readback;
    ComPtr<ID3D12DescriptorHeap> cpuHeap;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};
    NrPadLayout pad{};
    unsigned frameWidth=0, frameHeight=0, descriptor=0;
    bool haveDescriptor=false, checkPending=false;
    int checkFrame=-1;
};
