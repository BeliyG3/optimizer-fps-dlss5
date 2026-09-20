#include "device.h"
#include "image_mips.h"
#include <cstring>

ComPtr<ID3D12Resource> Device::UploadTexture(unsigned w, unsigned h, DXGI_FORMAT format, unsigned pixelBytes, const void *data)
{
    return UploadMips(format,pixelBytes,BuildMips(w,h,pixelBytes,data,format==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB));
}
// The mip chain is built by the caller: filtering is the slow part of loading a scene and runs on
// worker threads, while everything below touches the device and stays on the calling thread.
ComPtr<ID3D12Resource> Device::UploadMips(DXGI_FORMAT format, unsigned pixelBytes, const std::vector<ImageMip> &levels)
{
    if(levels.empty()) throw std::runtime_error("Texture without mip levels");
    const unsigned w=levels[0].width, h=levels[0].height;
    unsigned count=unsigned(levels.size());
    auto result=Texture(w,h,format,D3D12_RESOURCE_FLAG_NONE,D3D12_RESOURCE_STATE_COPY_DEST,count);
    auto desc=result->GetDesc(); std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> layouts(count); UINT64 bytes=0;
    gpu->GetCopyableFootprints(&desc,0,count,0,layouts.data(),nullptr,nullptr,&bytes);
    auto upload=Buffer(bytes,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
    void *p=nullptr; D3D12_RANGE empty{0,0}; Check(upload->Map(0,&empty,&p),"Map mip upload");
    for(unsigned i=0;i<count;++i) for(unsigned y=0;y<levels[i].height;++y)
        std::memcpy(static_cast<BYTE *>(p)+layouts[i].Offset+size_t(y)*layouts[i].Footprint.RowPitch,
            levels[i].bytes.data()+size_t(y)*levels[i].width*pixelBytes,size_t(levels[i].width)*pixelBytes);
    upload->Unmap(0,nullptr); Begin();
    for(unsigned i=0;i<count;++i) {
        D3D12_TEXTURE_COPY_LOCATION src{},dst{};
        src.pResource=upload.Get(); src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.PlacedFootprint=layouts[i];
        dst.pResource=result.Get(); dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dst.SubresourceIndex=i;
        list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
    }
    Transition(list.Get(),result.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Submit(); return result;
}
