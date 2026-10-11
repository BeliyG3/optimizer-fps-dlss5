#include "raw_dump.h"
#include <DirectXPackedVector.h>
#include <cstdint>
#include <cstring>
#include <fstream>

void RawDump::Queue(Device &d, ID3D12Resource *guide)
{
    auto desc=guide->GetDesc(); UINT64 bytes=0;
    depth=desc.Format==DXGI_FORMAT_R32_FLOAT;
    if(!depth && desc.Format!=DXGI_FORMAT_R16G16_FLOAT) throw std::runtime_error("--dump-raw expects an R16G16_FLOAT motion or R32_FLOAT depth target");
    d.gpu->GetCopyableFootprints(&desc,0,1,0,&layout,nullptr,nullptr,&bytes);
    readback=d.Buffer(bytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
    Transition(d.list.Get(),guide,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION src{},dst{}; src.pResource=guide; src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.pResource=readback.Get(); dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint=layout;
    d.list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
    Transition(d.list.Get(),guide,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}
void RawDump::Write(int frame,bool prevDepth) const
{
    if(!readback) throw std::runtime_error("No queued raw dump");
    const uint32_t width=layout.Footprint.Width, height=layout.Footprint.Height, channels=depth ? 1 : 2;
    std::vector<float> values(size_t(width)*height*channels); void *p=nullptr;
    D3D12_RANGE range{0,SIZE_T(readback->GetDesc().Width)}; Check(readback->Map(0,&range,&p),"Map raw dump");
    for(uint32_t y=0;y<height;++y) {
        const BYTE *row=static_cast<const BYTE *>(p)+layout.Offset+size_t(y)*layout.Footprint.RowPitch;
        float *out=values.data()+size_t(y)*width*channels;
        if(depth) std::memcpy(out,row,size_t(width)*sizeof(float));
        else for(size_t i=0;i<size_t(width)*2;++i) out[i]=DirectX::PackedVector::XMConvertHalfToFloat(reinterpret_cast<const DirectX::PackedVector::HALF *>(row)[i]);
    }
    D3D12_RANGE empty{0,0}; readback->Unmap(0,&empty);
    if(prevDepth) for(size_t i=0;i<size_t(width)*height;++i) values[i]=values[2*i]; // keep x, the depth
    if(prevDepth) values.resize(size_t(width)*height);
    const uint32_t header[4]={0,width,height,uint32_t(frame)}; char bytes[16]; std::memcpy(bytes,header,16);
    std::memcpy(bytes,prevDepth ? "OFPD" : depth ? "OFDP" : "OFMV",4);
    const auto path=ExecutableDirectory()/((prevDepth ? "prevdepth_" : depth ? "depth_" : "motion_")+std::to_string(frame)+".f32");
    std::ofstream out(path,std::ios::binary);
    out.write(bytes,16); out.write(reinterpret_cast<const char *>(values.data()),std::streamsize(values.size()*sizeof(float)));
    if(!out) throw std::runtime_error("Write "+path.string());
    std::printf("[info] wrote %s\n",path.string().c_str());
}
