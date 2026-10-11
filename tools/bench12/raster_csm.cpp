// --render raster --csm: the sun shadow in four cascades fitted to the view each frame (raster_csm.h), drawn into the
// slices of one depth array and read by the CSM variants of the scene shaders (shaders/raster_csm.hlsli). With
// --lightmap a second array holds the triangles without baked light alone, for the baked sun on static surfaces.
#include "raster.h"
#include "raster_csm.h"
#include <cstring>

namespace {
constexpr unsigned Slices=4;
constexpr float CameraNear=0.1f; // Perspective's near plane (math.h)
}

// An R32 typeless array of `slices`: written through a D32 view per slice (DSVs firstDsv on), read as one R32_FLOAT
// array SRV. It rests readable by the scene pixel shader, as the single map does.
ComPtr<ID3D12Resource> Raster::DepthArray(Device &d, unsigned size, unsigned slices, D3D12_CPU_DESCRIPTOR_HANDLE firstDsv, unsigned dsvStep,
                                          unsigned srv)
{
    D3D12_HEAP_PROPERTIES heap{}; heap.Type=D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{}; desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D; desc.Width=size; desc.Height=size;
    desc.DepthOrArraySize=UINT16(slices); desc.MipLevels=1; desc.Format=DXGI_FORMAT_R32_TYPELESS; desc.SampleDesc.Count=1;
    desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE clear{}; clear.Format=DXGI_FORMAT_D32_FLOAT; clear.DepthStencil.Depth=1;
    ComPtr<ID3D12Resource> r;
    Check(d.gpu->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,&clear,IID_PPV_ARGS(&r)),
          "Create raster shadow map array");
    for(unsigned c=0;c<slices;++c) {
        D3D12_DEPTH_STENCIL_VIEW_DESC v{}; v.Format=DXGI_FORMAT_D32_FLOAT; v.ViewDimension=D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
        v.Texture2DArray.FirstArraySlice=c; v.Texture2DArray.ArraySize=1;
        d.gpu->CreateDepthStencilView(r.Get(),&v,{firstDsv.ptr+c*dsvStep});
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC s{}; s.Format=DXGI_FORMAT_R32_FLOAT; s.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    s.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; s.Texture2DArray.MipLevels=1; s.Texture2DArray.ArraySize=slices;
    d.gpu->CreateShaderResourceView(r.Get(),&s,d.Cpu(srv));
    return r;
}

void Raster::UseCsm(Device &d, const Options &o)
{
    RequireCreated();
    auto next=std::make_unique<Csm>();
    CreateCsmPipelines(d,*next);
    D3D12_DESCRIPTOR_HEAP_DESC heap{}; heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_DSV; heap.NumDescriptors=baked ? 2*Slices : Slices;
    Check(d.gpu->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&next->dsvHeap)),"Create raster cascade DSV heap");
    const auto dsv=next->dsvHeap->GetCPUDescriptorHandleForHeapStart();
    next->srv=d.Allocate(1); // t3 space2
    next->array=DepthArray(d,ShadowResolution,Slices,dsv,dsvStep,next->srv);
    if(baked) {
        next->dynamicSrv=d.Allocate(1); // t1 space3, in place of the dynamic-only map
        next->dynamicArray=DepthArray(d,ShadowResolution,Slices,{dsv.ptr+Slices*dsvStep},dsvStep,next->dynamicSrv);
    }
    next->constants=d.Buffer(512,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
    void *p=nullptr; D3D12_RANGE empty{0,0}; Check(next->constants->Map(0,&empty,&p),"Map raster cascade constants");
    next->mapped=static_cast<CsmConstants *>(p);
    csm=std::move(next); // only complete: Record tests `csm` alone
    std::printf("[raster] csm: %u cascades of %u^2 to %.0f m, %s%s\n",Slices,ShadowResolution,o.csmDistance,
                o.csmStable ? "stable" : "unstable",baked ? "; the dynamic-only sun shadow in cascades too" : "");
}
void Raster::WriteCascades(const Scene &s, const Options &o, const FrameConstants &c, D3D12_GPU_VIRTUAL_ADDRESS frameConstants)
{
    // The unjittered camera currentVP was built from (FrameConstants), so the cascades do not follow the jitter.
    const CascadeSet set=FitCascades(c.eye,c.forward,c.up,c.tanHalf,c.aspect,CameraNear,o.csmDistance,c.sunTravel,
                                     s.boundsMin,s.boundsMax,ShadowResolution,o.csmStable);
    CsmConstants k{};
    for(unsigned i=0;i<Slices;++i) {
        const Cascade &cascade=set.cascades[i];
        k.cascadeVP[i]=cascade.viewProjection; csm->viewProjection[i]=cascade.viewProjection; k.splitFar[i]=cascade.splitFar; k.texelWorld[i]=cascade.texelWorld;
        // One texel of depth bias in world units, as WriteConstants gives the single map.
        k.bias[i]=cascade.depthRange>0 ? cascade.texelWorld/cascade.depthRange : 0;
    }
    k.forward=Normalize(c.forward); k.blend=set.blendFraction;
    // Device::Submit fences every frame, so the previous frame's constants are no longer read.
    std::memcpy(csm->mapped,&k,sizeof(k));
    csm->frameConstants=frameConstants;
}
void Raster::DrawCascades(Device &d, const Scene &s, ID3D12Resource *array, bool dynamicOnly)
{
    auto *list=d.list.Get();
    Transition(list,array,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_DEPTH_WRITE);
    list->SetGraphicsRootSignature(csm->root.Get());
    BindScene(d,s,csm->frameConstants);
    list->SetGraphicsRootConstantBufferView(csm->firstParameter,csm->constants->GetGPUVirtualAddress());
    list->SetPipelineState(dynamicOnly ? csm->dynamicShadowPso.Get() : csm->shadowPso.Get());
    const D3D12_VIEWPORT viewport{0,0,float(ShadowResolution),float(ShadowResolution),0,1};
    const D3D12_RECT scissor{0,0,LONG(ShadowResolution),LONG(ShadowResolution)};
    list->RSSetViewports(1,&viewport); list->RSSetScissorRects(1,&scissor);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const auto first=csm->dsvHeap->GetCPUDescriptorHandleForHeapStart().ptr+(dynamicOnly ? Slices : 0)*dsvStep;
    // raster_csm_shadow_dynamic_vs.cso offsets the vertex id by dynamicBase*3, as raster_shadow_dynamic_vs.cso does;
    // raster_csm_shadow_list_vs.cso walks Scene::unbakedTriangles (static triangles without lightmap UVs, then the dynamic ones).
    if(baked) list->SetGraphicsRootShaderResourceView(BakedTriangleList,s.unbakedTriangles->GetGPUVirtualAddress());
    const unsigned triangles=dynamicOnly ? (baked->listShadow ? s.unbakedTriangleCount : s.dynamicTriangles) : 0;
    for(unsigned c=0;c<Slices;++c) {
        const D3D12_CPU_DESCRIPTOR_HANDLE dsv{first+c*dsvStep};
        list->OMSetRenderTargets(0,nullptr,FALSE,&dsv);
        list->ClearDepthStencilView(dsv,D3D12_CLEAR_FLAG_DEPTH,1,0,0,nullptr);
        list->SetGraphicsRoot32BitConstant(csm->firstParameter+2,c,0); // cascadeIndex (b3 space1)
        if(!dynamicOnly) RasterInstances::Draw(list,s,CullShadow(s,csm->viewProjection[c],false));
        else if(triangles) list->DrawInstanced(triangles*3,1,0,0);
    }
    Transition(list,array,D3D12_RESOURCE_STATE_DEPTH_WRITE,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}
void Raster::BindCsm(Device &d, const Scene &s, ID3D12RootSignature *root)
{
    // The scene pass on the CSM root: BindScene's parameters (DrawScene sets 5, the unused single map), with --lightmap
    // the baked ones (the dynamic shadow is the dynamic cascade array), then the cascades.
    auto *list=d.list.Get();
    list->SetGraphicsRootSignature(root ? root : csm->root.Get());
    BindScene(d,s,csm->frameConstants);
    if(baked) BindBakedParameters(d,s,csm->dynamicSrv);
    list->SetGraphicsRootConstantBufferView(csm->firstParameter,csm->constants->GetGPUVirtualAddress());
    list->SetGraphicsRootDescriptorTable(csm->firstParameter+1,d.Gpu(csm->srv));
}
