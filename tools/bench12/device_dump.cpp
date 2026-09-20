#include "device.h"
#include <fstream>
#include <cstring>

void Device::QueueDump()
{
    auto desc=BackBuffer()->GetDesc(); UINT64 bytes=0;
    gpu->GetCopyableFootprints(&desc,0,1,0,&dumpLayout,nullptr,nullptr,&bytes);
    if(!readback) readback=Buffer(bytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
    Transition(list.Get(),BackBuffer(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION src{},dst{}; src.pResource=BackBuffer(); src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.pResource=readback.Get(); dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint=dumpLayout;
    list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
    Transition(list.Get(),BackBuffer(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);
}
void Device::WriteDump(int frame)
{
    if(!readback) throw std::runtime_error("No queued dump");
    // The back buffer is already sRGB encoded. Only swizzle RGBA to BMP's BGRA.
    std::vector<BYTE> pixels(size_t(width)*height*4); void *p=nullptr;
    D3D12_RANGE range{0,SIZE_T(readback->GetDesc().Width)}; Check(readback->Map(0,&range,&p),"Map dump");
    for(unsigned y=0;y<height;++y) {
        const BYTE *src=static_cast<const BYTE *>(p)+dumpLayout.Offset+size_t(y)*dumpLayout.Footprint.RowPitch;
        BYTE *dst=pixels.data()+size_t(y)*width*4;
        for(unsigned x=0;x<width;++x) { dst[x*4]=src[x*4+2]; dst[x*4+1]=src[x*4+1]; dst[x*4+2]=src[x*4]; dst[x*4+3]=255; }
    }
    D3D12_RANGE empty{0,0}; readback->Unmap(0,&empty);
    BITMAPFILEHEADER file{}; file.bfType=0x4d42; file.bfOffBits=sizeof(file)+sizeof(BITMAPINFOHEADER); file.bfSize=file.bfOffBits+DWORD(pixels.size());
    BITMAPINFOHEADER info{}; info.biSize=sizeof(info); info.biWidth=LONG(width); info.biHeight=-LONG(height);
    info.biPlanes=1; info.biBitCount=32; info.biCompression=BI_RGB; info.biSizeImage=DWORD(pixels.size());
    auto path=ExecutableDirectory()/("dump_"+std::to_string(frame)+".bmp"); std::ofstream stream(path,std::ios::binary);
    stream.write(reinterpret_cast<const char *>(&file),sizeof(file)); stream.write(reinterpret_cast<const char *>(&info),sizeof(info));
    stream.write(reinterpret_cast<const char *>(pixels.data()),std::streamsize(pixels.size())); stream.close();
    if(!stream) throw std::runtime_error("Cannot write dump: "+path.string());
    std::printf("[info] wrote %s\n",path.string().c_str());
}
