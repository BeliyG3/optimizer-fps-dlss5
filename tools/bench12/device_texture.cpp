#include "device.h"
#include "image_mips.h"
#include <cstring>

ComPtr<ID3D12Resource> Device::UploadTexture(unsigned w, unsigned h, DXGI_FORMAT format, unsigned pixelBytes, const void *data)
{
    return UploadMips(format,pixelBytes,BuildMips(w,h,pixelBytes,data,format==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB));
}
ComPtr<ID3D12Resource> Device::UploadMips(DXGI_FORMAT format, unsigned pixelBytes, const std::vector<ImageMip> &levels)
{
    std::vector<StagedTexture> one; one.push_back(StageMips(format,pixelBytes,levels));
    CopyStaged(one); return one[0].texture;
}
// The mip chain is built by the caller: filtering is the slow part of loading a scene and runs on worker threads. So does
// this (device creation is free-threaded): the texture, its own upload buffer and the levels copied into it.
StagedTexture Device::StageMips(DXGI_FORMAT format, unsigned pixelBytes, const std::vector<ImageMip> &levels)
{
    if(levels.empty()) throw std::runtime_error("Texture without mip levels");
    StagedTexture s;
    const unsigned count=unsigned(levels.size());
    s.texture=Texture(levels[0].width,levels[0].height,format,D3D12_RESOURCE_FLAG_NONE,D3D12_RESOURCE_STATE_COPY_DEST,count);
    auto desc=s.texture->GetDesc(); s.layouts.resize(count);
    gpu->GetCopyableFootprints(&desc,0,count,0,s.layouts.data(),nullptr,nullptr,&s.bytes);
    s.upload=Buffer(s.bytes,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
    void *p=nullptr; D3D12_RANGE empty{0,0}; Check(s.upload->Map(0,&empty,&p),"Map mip upload");
    for(unsigned i=0;i<count;++i) for(unsigned y=0;y<levels[i].height;++y)
        std::memcpy(static_cast<BYTE *>(p)+s.layouts[i].Offset+size_t(y)*s.layouts[i].Footprint.RowPitch,
            levels[i].bytes.data()+size_t(y)*levels[i].width*pixelBytes,size_t(levels[i].width)*pixelBytes);
    s.upload->Unmap(0,nullptr);
    return s;
}
// On the device's thread: one list copies the batch and waits; the upload buffers are released with it.
void Device::CopyStaged(std::vector<StagedTexture> &batch)
{
    Begin();
    for(auto &s:batch) {
        for(unsigned i=0;i<unsigned(s.layouts.size());++i) {
            D3D12_TEXTURE_COPY_LOCATION src{},dst{};
            src.pResource=s.upload.Get(); src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.PlacedFootprint=s.layouts[i];
            dst.pResource=s.texture.Get(); dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dst.SubresourceIndex=i;
            list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
        }
        Transition(list.Get(),s.texture.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    Submit();
    for(auto &s:batch) { s.upload.Reset(); s.layouts.clear(); }
}
