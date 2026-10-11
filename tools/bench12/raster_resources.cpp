#include "raster.h"
#include <cstring>

namespace {
constexpr auto PixelRead=D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
ComPtr<ID3D12Resource> Target(Device &d, unsigned w, unsigned h, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags,
                              D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE &clear)
{
    D3D12_HEAP_PROPERTIES heap{}; heap.Type=D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{}; desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D; desc.Width=w; desc.Height=h;
    desc.DepthOrArraySize=1; desc.MipLevels=1; desc.Format=format; desc.SampleDesc.Count=1; desc.Flags=flags;
    ComPtr<ID3D12Resource> r;
    Check(d.gpu->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,state,&clear,IID_PPV_ARGS(&r)),"Create raster target");
    return r;
}
D3D12_CLEAR_VALUE ColourClear(DXGI_FORMAT format) { D3D12_CLEAR_VALUE c{}; c.Format=format; return c; } // zero
D3D12_CLEAR_VALUE DepthClear(float depth) { D3D12_CLEAR_VALUE c{}; c.Format=DXGI_FORMAT_D32_FLOAT; c.DepthStencil.Depth=depth; return c; }
void Srv(Device &d, ID3D12Resource *r, DXGI_FORMAT format, unsigned descriptor)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC s{}; s.Format=format; s.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
    s.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; s.Texture2D.MipLevels=1;
    d.gpu->CreateShaderResourceView(r,&s,d.Cpu(descriptor));
}
// A typeless R32 depth buffer: written through a D32 view, read by shaders as R32_FLOAT.
ComPtr<ID3D12Resource> DepthTarget(Device &d, unsigned w, unsigned h, float clear, D3D12_RESOURCE_STATES state,
                                   D3D12_CPU_DESCRIPTOR_HANDLE dsv, unsigned srv)
{
    auto r=Target(d,w,h,DXGI_FORMAT_R32_TYPELESS,D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL,state,DepthClear(clear));
    D3D12_DEPTH_STENCIL_VIEW_DESC v{}; v.Format=DXGI_FORMAT_D32_FLOAT; v.ViewDimension=D3D12_DSV_DIMENSION_TEXTURE2D;
    d.gpu->CreateDepthStencilView(r.Get(),&v,dsv); Srv(d,r.Get(),DXGI_FORMAT_R32_FLOAT,srv);
    return r;
}
}

void Raster::CreateDescriptors(Device &d)
{
    D3D12_DESCRIPTOR_HEAP_DESC heap{}; heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV; heap.NumDescriptors=2; // colour, motion
    Check(d.gpu->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&rtvHeap)),"Create raster RTV heap");
    heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_DSV; // scene depth, sun shadow map
    Check(d.gpu->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&dsvHeap)),"Create raster DSV heap");
    rtvStep=d.gpu->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    dsvStep=d.gpu->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    srvBase=d.Allocate(4); // t1..t4 space2: shadow map, colour, depth, motion
    constants=d.Buffer(256,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
    void *p=nullptr; D3D12_RANGE empty{0,0}; Check(constants->Map(0,&empty,&p),"Map raster constants");
    mappedConstants=static_cast<Constants *>(p);
}
void Raster::CreateBakedTargets(Device &d, Baked &b)
{
    // --lightmap: a DSV heap and an SRV of their own, so a run without it allocates nothing new.
    D3D12_DESCRIPTOR_HEAP_DESC heap{}; heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_DSV; heap.NumDescriptors=1;
    Check(d.gpu->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&b.dsvHeap)),"Create raster dynamic shadow DSV heap");
    b.shadowSrv=d.Allocate(1); // t1 space3
    b.shadowMap=DepthTarget(d,ShadowResolution,ShadowResolution,1,PixelRead,b.dsvHeap->GetCPUDescriptorHandleForHeapStart(),b.shadowSrv);
}
void Raster::Resize(Device &, unsigned w, unsigned h)
{
    // Called where the path tracer's outputs are recreated, after the previous frame's fence: the old targets have
    // retired. The new ones are created by the next raster frame, so a path-traced session never allocates them.
    width=w; height=h; colourTarget.Reset(); motionTarget.Reset(); depthTarget.Reset();
    if(ao) ao->Release();
}
void Raster::EnsureTargets(Device &d, bool reverse)
{
    auto dsv=dsvHeap->GetCPUDescriptorHandleForHeapStart(), rtv=rtvHeap->GetCPUDescriptorHandleForHeapStart();
    D3D12_CPU_DESCRIPTOR_HANDLE shadowDsv{dsv.ptr+dsvStep}, motionRtv{rtv.ptr+rtvStep};
    // Resting states between frames: the shadow map is read by the scene pixel shader, the targets by the resolve.
    if(!shadowMap) shadowMap=DepthTarget(d,ShadowResolution,ShadowResolution,1,PixelRead,shadowDsv,srvBase);
    if(colourTarget && targetsReverse==reverse) return;
    if(!width || !height) throw std::logic_error("Raster::Resize was not called before the first raster frame");
    const auto rt=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    const auto read=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    colourTarget=Target(d,width,height,DXGI_FORMAT_R16G16B16A16_FLOAT,rt,read,ColourClear(DXGI_FORMAT_R16G16B16A16_FLOAT));
    d.gpu->CreateRenderTargetView(colourTarget.Get(),nullptr,rtv); Srv(d,colourTarget.Get(),DXGI_FORMAT_R16G16B16A16_FLOAT,srvBase+1);
    depthTarget=DepthTarget(d,width,height,reverse ? 0.0f : 1.0f,read,dsv,srvBase+2);
    motionTarget=Target(d,width,height,DXGI_FORMAT_R16G16_FLOAT,rt,read,ColourClear(DXGI_FORMAT_R16G16_FLOAT));
    d.gpu->CreateRenderTargetView(motionTarget.Get(),nullptr,motionRtv); Srv(d,motionTarget.Get(),DXGI_FORMAT_R16G16_FLOAT,srvBase+3);
    targetsReverse=reverse;
    std::printf("[raster] targets %ux%u, %s depth, shadow map %u^2\n",width,height,reverse ? "reverse" : "standard",ShadowResolution);
}
void Raster::UploadLights(Device &d, const Scene &s)
{
    // Lamp group intensity and tint change the lights' power: rebuild after every Scene::UpdateLamps.
    if(lightsBuilt && lampVersion==s.lampVersion) return;
    std::vector<unsigned> pointGroups, lineGroups;
    const auto built=tubes ? UploadTubes(d,s,pointGroups,lineGroups) : BuildRasterLights(s.lampSources,s.lampCentroids,s.groups,&pointGroups);
    const unsigned needed=std::max(1u,unsigned(built.size()));
    if(needed>lightCapacity) {
        // Device::Submit fences every frame, so the previous buffer has retired and may be released here.
        lights=d.Buffer(UINT64(needed)*sizeof(RasterLight),D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
        void *p=nullptr; D3D12_RANGE empty{0,0}; Check(lights->Map(0,&empty,&p),"Map raster lights");
        mappedLights=static_cast<RasterLight *>(p); lightCapacity=needed;
    }
    if(!built.empty()) std::memcpy(mappedLights,built.data(),built.size()*sizeof(RasterLight));
    Vec3 total{}; for(const auto &light:built) total=total+light.power;
    if(!lightsBuilt || lightCount!=built.size())
        std::printf("[raster] %zu lamp lights, total power %.1f %.1f %.1f\n",built.size(),total.x,total.y,total.z);
    lightCount=unsigned(built.size()); lampVersion=s.lampVersion; lightsBuilt=true;
    if(lampShadows) UploadLampShadowSlices(d,pointGroups,lineGroups);
}
