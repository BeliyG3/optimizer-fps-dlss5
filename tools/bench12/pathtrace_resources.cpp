#include "pathtrace.h"

ID3D12Resource *Pathtrace::DisplaySource(const Options &o) const
{
    if(o.view=="noisy") return outputs[0].Get();
    if(o.view=="accum" || (o.view=="colour" && o.accumulationEnabled && (!o.feedAccumulation || !ngx.Output()))) return outputs[7].Get();
    if(auto *neural=nr.Presented()) return neural;
    return ngx.Output() ? ngx.Output() : outputs[0].Get();
}
void Pathtrace::ConfigureNeuralRendering(Device &d, Options &o)
{
    if(o.mvFormat=="rgba16f") motionPack.Configure(d,outputs[2].Get(),width,height); else motionPack.Disable();
    // Native NR runs on the image it follows: the upscaler output, or the render colour without one.
    auto *colour=ngx.Output() ? ngx.Output() : outputs[0].Get();
    try { nr.Configure(d,o,colour,width,height); }
    catch(const std::exception &failure) {
        if(!o.interactive) throw;
        error=failure.what(); o.nr="off"; nr.Configure(d,o,colour,width,height);
        std::fprintf(stderr,"[nr fallback] %s\n",error.c_str());
    }
    configuredNr=o.nr; configuredMotion=o.mvFormat;
}
void Pathtrace::BindDisplay(Device &d, const Options &o)
{
    auto *source=DisplaySource(o);
    std::string key=std::to_string(reinterpret_cast<uintptr_t>(source));
    if(displayKey!=key) { display.Initialize(d,source,srvBase+8); displayKey=key; }
    d.TextureSrv(source,srvBase);
}
void Pathtrace::Configure(Device &d, Options &o)
{
    unsigned w=std::max(1u,unsigned(float(d.width)*o.renderScale)), h=std::max(1u,unsigned(float(d.height)*o.renderScale));
    bool resized=w!=width || h!=height || outputWidth!=d.width || outputHeight!=d.height;
    if(resized || configuredUpscaler!=o.upscaler || configuredReverse!=o.reverse || configuredNr!=o.nr || configuredMotion!=o.mvFormat) {
        nr.Shutdown(d); ngx.Shutdown(d); Reset(); initialized=false;
        width=w; height=h; outputWidth=d.width; outputHeight=d.height;
        const DXGI_FORMAT formats[]={DXGI_FORMAT_R16G16B16A16_FLOAT,DXGI_FORMAT_R32_FLOAT,DXGI_FORMAT_R16G16_FLOAT,
            DXGI_FORMAT_R16G16B16A16_FLOAT,DXGI_FORMAT_R16G16B16A16_FLOAT,DXGI_FORMAT_R16G16B16A16_FLOAT,
            DXGI_FORMAT_R16_FLOAT,DXGI_FORMAT_R32G32B32A32_FLOAT,DXGI_FORMAT_R16G16B16A16_FLOAT};
        for(unsigned i=0;i<9;++i) {
            D3D12_FEATURE_DATA_FORMAT_SUPPORT support{formats[i]};
            Check(d.gpu->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT,&support,sizeof(support)),"Check guide format");
            if(!(support.Support2&D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) || (i==7 && !(support.Support2&D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD)))
                throw std::runtime_error("Device does not support required typed UAV guide/accumulation format");
            outputs[i]=d.Texture(width,height,formats[i],D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            D3D12_UNORDERED_ACCESS_VIEW_DESC u{}; u.Format=formats[i]; u.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
            d.gpu->CreateUnorderedAccessView(outputs[i].Get(),nullptr,&u,d.Cpu(uavBase+(i==8 ? 9 : i)));
            if(i<8) d.TextureSrv(outputs[i].Get(),srvBase+i);
        }
        std::printf("[info] render %ux%u, output %ux%u\n",width,height,d.width,d.height);
        error.clear();
        // NR upscaling replaces DLSS; the options reject the pair, the interactive menu may still set it.
        if(o.upscaler!="none" && o.nr!="upscale") {
            try { ngx.Initialize(d,o,width,height); }
            catch(const std::exception &failure) {
                if(!o.interactive) throw;
                error=failure.what(); ngx.Shutdown(d); o.upscaler="none";
                std::fprintf(stderr,"[ngx fallback] %s\n",error.c_str());
            }
        }
        configuredUpscaler=o.upscaler; configuredReverse=o.reverse; displayKey.clear();
        ConfigureNeuralRendering(d,o);
    }
}
bool Pathtrace::ReadPick(unsigned &triangle, unsigned &group)
{
    if(!pickPending) return false;
    // Render's Submit fence has retired this copy; consume it at the next UI frame.
    void *p=nullptr; D3D12_RANGE range{0,8};
    Check(pickReadback->Map(0,&range,&p),"Map GPU pick");
    const auto *result=static_cast<const unsigned *>(p); triangle=result[0]; group=result[1];
    D3D12_RANGE empty{0,0}; pickReadback->Unmap(0,&empty); pickPending=false; return true;
}
