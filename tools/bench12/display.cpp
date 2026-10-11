#include "display.h"
#include "pipeline.h"
#include "shaders/display_math.hlsli"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace {
constexpr auto ReadState=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE|D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
constexpr unsigned MeterWords=METER_BINS+3, Constants=22; // the histogram, then the scale and the two targets
void Uav(Device &d, ID3D12Resource *resource, unsigned descriptor)
{
    D3D12_UNORDERED_ACCESS_VIEW_DESC view{}; view.Format=resource->GetDesc().Format;
    view.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
    d.gpu->CreateUnorderedAccessView(resource,nullptr,&view,d.Cpu(descriptor));
}
}
void Display::Initialize(Device &d, ID3D12Resource *source, unsigned presentDescriptors)
{
    levels.clear(); initialized=false;
    sourceWidth=unsigned(source->GetDesc().Width); sourceHeight=source->GetDesc().Height;
    unsigned w=sourceWidth,h=sourceHeight;
    do {
        w=std::max(1u,(w+1)/2); h=std::max(1u,(h+1)/2);
        levels.push_back(d.Texture(w,h,DXGI_FORMAT_R32G32B32A32_FLOAT,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,ReadState));
    } while(w>1 || h>1);
    if(!descriptors) descriptors=d.Allocate(32);
    for(unsigned i=0;i<levels.size();++i) {
        d.TextureSrv(i ? levels[i-1].Get() : source,descriptors+i*2);
        Uav(d,levels[i].Get(),descriptors+i*2+1);
    }
    // Broad copies at 1/4, 1/16, and 1/64 resolution; bilinear reconstruction
    // and the repeated box filters blur without thresholding bright emitters.
    for(unsigned i=0;i<3;++i) d.TextureSrv(levels[std::min(size_t(1+i*2),levels.size()-1)].Get(),presentDescriptors+i);
    exposure=d.Texture(1,1,DXGI_FORMAT_R32G32B32A32_FLOAT,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,ReadState);
    if(!exposureUav) exposureUav=d.Allocate(2); d.TextureSrv(levels.back().Get(),exposureUav); Uav(d,exposure.Get(),exposureUav+1);
    d.TextureSrv(exposure.Get(),presentDescriptors+3);
    std::vector<unsigned> widths; for(const auto &level:levels) widths.push_back(unsigned(level->GetDesc().Width));
    local.Initialize(d,source,exposure.Get(),levels[LocalExposureBlurLevel(widths)].Get(),presentDescriptors);
    // The histogram meters the first level at most 512 texels wide (1/8 of 4K): enough pixels, little bandwidth.
    meterLevel=0; while(meterLevel+2<levels.size() && levels[meterLevel]->GetDesc().Width>512) ++meterLevel;
    if(!meter) {
        meter=d.Buffer(MeterWords*4,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        meterReadback=d.Buffer(MeterWords*4,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
        meterUav=d.Allocate(1);
        D3D12_UNORDERED_ACCESS_VIEW_DESC u{}; u.ViewDimension=D3D12_UAV_DIMENSION_BUFFER;
        u.Buffer.NumElements=MeterWords; u.Buffer.StructureByteStride=4;
        d.gpu->CreateUnorderedAccessView(meter.Get(),nullptr,&u,d.Cpu(meterUav));
    }
    D3D12_DESCRIPTOR_RANGE sr{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,0,0,0}, ur{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,0,0,0};
    D3D12_DESCRIPTOR_RANGE mr{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,1,0,0};
    D3D12_ROOT_PARAMETER parameters[4]{};
    parameters[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; parameters[0].Constants={0,0,Constants};
    parameters[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; parameters[1].DescriptorTable={1,&sr};
    parameters[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; parameters[2].DescriptorTable={1,&ur};
    parameters[3].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; parameters[3].DescriptorTable={1,&mr};
    D3D12_ROOT_SIGNATURE_DESC desc{}; desc.NumParameters=4; desc.pParameters=parameters; root=Root(d,desc);
    auto make=[&](const char *name, ComPtr<ID3D12PipelineState> &pso) {
        auto shader=Shader(name); D3D12_COMPUTE_PIPELINE_STATE_DESC p{};
        p.pRootSignature=root.Get(); p.CS={shader.data(),shader.size()};
        Check(d.gpu->CreateComputePipelineState(&p,IID_PPV_ARGS(&pso)),"Create display compute PSO");
    };
    make("display_down.cso",downsample); make("display_histogram.cso",histogram); make("display_adapt.cso",adapt);
}
void Display::Prepare(Device &d, ID3D12Resource *source, const ExposureControl &control)
{
    // Source starts and ends as a pixel SRV, including NGX's distinct output.
    Transition(d.list.Get(),source,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,ReadState);
    d.list->SetComputeRootSignature(root.Get()); d.list->SetPipelineState(downsample.Get());
    d.list->SetComputeRootDescriptorTable(3,d.Gpu(meterUav));
    unsigned w=sourceWidth,h=sourceHeight;
    for(unsigned i=0;i<levels.size();++i) {
        auto *level=levels[i].Get();
        Transition(d.list.Get(),level,ReadState,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        unsigned data[Constants]={w,h,sourceWidth,sourceHeight,1u<<i,i==0 ? 1u : 0u,0,0};
        d.list->SetComputeRoot32BitConstants(0,Constants,data,0);
        d.list->SetComputeRootDescriptorTable(1,d.Gpu(descriptors+i*2));
        d.list->SetComputeRootDescriptorTable(2,d.Gpu(descriptors+i*2+1));
        w=std::max(1u,(w+1)/2); h=std::max(1u,(h+1)/2);
        d.list->Dispatch((w+7)/8,(h+7)/8,1);
        Transition(d.list.Get(),level,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,ReadState);
    }
    unsigned data[Constants]={1,1,sourceWidth,sourceHeight,1,0,initialized ? 0u : 1u,control.automatic ? 1u : 0u};
    const float evScale=std::exp2(control.ev);
    std::memcpy(data+8,&control.seconds,4); std::memcpy(data+9,&control.speedUp,4); std::memcpy(data+10,&control.speedDown,4);
    std::memcpy(data+11,&evScale,4); std::memcpy(data+12,&control.centre,4); data[13]=control.ue ? 1u : 0u;
    std::memcpy(data+14,&control.minScale,4); std::memcpy(data+15,&control.maxScale,4);
    std::memcpy(data+16,&control.exposure,4); std::memcpy(data+17,&control.curveLow,4);
    std::memcpy(data+18,&control.curveHigh,4); std::memcpy(data+19,&control.curveBias,4);
    data[20]=control.replay ? 1u : 0u; std::memcpy(data+21,&control.replayScale,4);
    // The metering level's texels into the histogram (the exposure UAV is bound but unused).
    const auto meterDesc=levels[meterLevel]->GetDesc();
    data[0]=unsigned(meterDesc.Width); data[1]=meterDesc.Height;
    d.list->SetPipelineState(histogram.Get()); d.list->SetComputeRoot32BitConstants(0,Constants,data,0);
    d.list->SetComputeRootDescriptorTable(1,d.Gpu(descriptors+(meterLevel+1)*2));
    d.list->SetComputeRootDescriptorTable(2,d.Gpu(exposureUav+1));
    d.list->Dispatch((data[0]+7)/8,(data[1]+7)/8,1);
    UavBarrier(d.list.Get(),meter.Get());
    data[0]=data[1]=1;
    Transition(d.list.Get(),exposure.Get(),ReadState,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    d.list->SetPipelineState(adapt.Get()); d.list->SetComputeRoot32BitConstants(0,Constants,data,0);
    d.list->SetComputeRootDescriptorTable(1,d.Gpu(exposureUav));
    d.list->SetComputeRootDescriptorTable(2,d.Gpu(exposureUav+1)); d.list->Dispatch(1,1,1);
    Transition(d.list.Get(),exposure.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,ReadState);
    Transition(d.list.Get(),meter.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
    d.list->CopyBufferRegion(meterReadback.Get(),0,meter.Get(),0,MeterWords*4);
    Transition(d.list.Get(),meter.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if(control.local) local.Record(d); // reads the source and this frame's exposure
    Transition(d.list.Get(),source,ReadState,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    initialized=metered=true;
}
ExposureReading Display::LastExposure() const
{
    ExposureReading r; if(!meterReadback || !metered) return r;
    void *p=nullptr; D3D12_RANGE range{METER_BINS*4,MeterWords*4}, empty{0,0};
    if(FAILED(meterReadback->Map(0,&range,&p))) return r;
    const auto *words=static_cast<const unsigned char *>(p)+METER_BINS*4;
    std::memcpy(&r.scale,words,4); std::memcpy(&r.average,words+4,4); std::memcpy(&r.highlight,words+8,4);
    meterReadback->Unmap(0,&empty); r.valid=true; return r;
}
