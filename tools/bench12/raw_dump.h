// --dump-raw motion|depth: render-size guides read back raw beside the exe, a 16-byte header
// {char magic[4]; uint32 width, height, frame;} then rows from the top.
// - motion (outputs[2], R16G16_FLOAT): motion_<frame>.f32, magic "OFMV", width*height float32 pairs (x, y):
//   current->previous, render pixels, +x right, +y down.
// - depth (outputs[1], R32_FLOAT): depth_<frame>.f32, magic "OFDP", width*height float32 in the bench's depth convention
//   (--depth: standard z/w by default).
// - prevdepth (--dump-raw prevdepth, the motion target with view mode 9): prevdepth_<frame>.f32, magic "OFPD", width*height
//   float32: the view depth (clip w, metres) the pixel's surface point had in the camera of frame t - SpanAt(t), at its
//   position then (animation and skinning included), occlusion ignored; 0 on the sky. The phase 3 disocclusion band.
#pragma once
#include "device.h"

class RawDump {
public:
    // Records the copy into a readback buffer; `guide` is in PIXEL_SHADER_RESOURCE state before and after.
    // The format picks the file: R16G16_FLOAT motion, R32_FLOAT depth.
    void Queue(Device &d, ID3D12Resource *guide);
    // After the frame's fence (Device::Submit waits for it): converts the halves (motion) and writes the file.
    // prevDepth: the motion target holds (span-back view depth, 0) -> prevdepth_<frame>.f32 ("OFPD", one float a pixel).
    void Write(int frame,bool prevDepth=false) const;
private:
    ComPtr<ID3D12Resource> readback;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};
    bool depth=false;
};
