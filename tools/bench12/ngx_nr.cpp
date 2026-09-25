#include "ngx_nr.h"
#include "ngx.h"

namespace {
constexpr auto ReadState=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
constexpr auto WriteState=D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
}
NeuralRendering::~NeuralRendering()
{
    // On an exception, drain before member destruction can release output/bridge resources.
    if(!coreSession.Close()) std::terminate();
}
void NeuralRendering::Configure(Device &d, const Options &o, ID3D12Resource *colour, unsigned rw, unsigned rh)
{
    Release(d);
    useCore=o.host=="core";
    mode=o.nr;
    if(mode=="off") return;
    const bool upscale=mode=="upscale";
    const auto colourDesc=colour->GetDesc();
    const unsigned cw=unsigned(colourDesc.Width), ch=colourDesc.Height;
    const NrPadLayout layout{unsigned(o.nrPadX),unsigned(o.nrPadY),unsigned(o.nrBaseX),unsigned(o.nrBaseY)};
    colourRect={0,0,cw,ch}; guideRect={0,0,rw,rh};
    outputRect={layout.baseX,layout.baseY,upscale ? d.width : cw,upscale ? d.height : ch};
    // The output stays a UAV for its whole life; NrColourBridge reads the region out of it.
    output=d.Texture(outputRect.width+layout.padX,outputRect.height+layout.padY,DXGI_FORMAT_R16G16B16A16_FLOAT,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,WriteState);
    pad.Configure(d,output.Get(),outputRect.width,outputRect.height,layout);
    bridge.Configure(d,colour,output.Get(),outputRect.width,outputRect.height,o.nrColour=="srgb",
        unsigned(o.nrColourPadX),unsigned(o.nrColourPadY),o.nrColourPadEdge!=0);
    if(o.nrColourPadX || o.nrColourPadY)
        std::printf("[nr] colour region %ux%u at 0,0 of a %ux%u texture\n",cw,ch,cw+unsigned(o.nrColourPadX),ch+unsigned(o.nrColourPadY));
    depthInverted=o.reverse;
    create={};
    if(upscale) {
        // The network input is the render size. PerfQualityValue and the render/output ratio are the
        // host's statement of intent; the 310.8 runtime keeps its network at the output rect.
        create.width=rw; create.height=rh;
        create.perfQuality=int(NgxQuality(o.renderScale));
        create.scalingRatio=float(rw)/float(outputRect.width);
    } else { create.width=cw; create.height=ch; }
    if(useCore) coreSession.Announce();
    if(!runtime.Loaded()) { runtime.Load(ExecutableDirectory(),d.gpu.Get(),o.nrLog); deferCreate=true; }
    if(useCore) {
        coreSession.Open(ExecutableDirectory()/L"pw_bench12.exe",d.gpu.Get(),d.Queue(),o.coreMode,o.coreTemporal,o.coreWarpPath);
        d.submissionContext=&coreSession;
        d.onSubmitted=[](void *context, ID3D12CommandQueue *queue, ID3D12CommandList *list) {
            static_cast<CoreSession *>(context)->Submitted(queue,list);
        };
    }
    if(upscale) std::printf("[nr] runtime scaling ratio for PerfQualityValue %d: %.3f (host ratio %.3f)\n",
        create.perfQuality,double(runtime.ScalingRatio(create.perfQuality)),double(create.scalingRatio));
}
void NeuralRendering::Prepare(Device &d, int frame)
{
    presented=false;
    pad.Report(); // the previous frame has been submitted and retired
    if(mode=="off" || (useCore ? coreSession.Ready() : feature)) return;
    if(deferCreate) {
        deferCreate=false;
        std::printf("[nr] feature 18 is created on frame %d, after the first present\n",frame+1); return;
    }
    d.Begin();
    NVSDK_NGX_Result result=NVSDK_NGX_Result_Success;
    int coreResult=OFPS_OK;
    if(useCore) {
        coreResult=coreSession.Create(d.list.Get(),d.gpu.Get(),create.width,create.height);
        result=coreSession.model.last;
    } else {
        parameters.Reset(); parameters.Audit(true);
        NrWriteCreate(parameters,create);
        result=runtime.Create(d.list.Get(),&parameters,&feature);
        parameters.Audit(false);
    }
    // Creation may record GPU initialization work. Retire it before testing the return code.
    d.Submit(); runtime.log.Echo();
    if(coreResult<0) throw std::runtime_error("core CreateFeature failed: "+std::to_string(coreResult));
    CheckNgx(result,"DLSSNR CreateFeature");
    if(!(useCore ? coreSession.Ready() : feature!=nullptr)) throw std::runtime_error("DLSSNR CreateFeature returned a null handle");
    std::printf("[nr] feature 18 created on frame %d: mode %s, feature %ux%u, result 0x%08X\n",
        frame,mode.c_str(),create.width,create.height,unsigned(result));
    pendingReset=true; calls=0;
}
void NeuralRendering::Evaluate(Device &d, const NrInputs &in, int frame)
{
    if(!(useCore ? coreSession.Ready() : feature!=nullptr)) return;
    auto *list=d.list.Get();
    // NR reads the sRGB proxy (a UAV like the guides) or, with --nr-colour linear, the colour itself.
    auto *colour=bridge.Encode(d,in.colourState);
    const auto colourState=bridge.Encoding() ? WriteState : in.colourState;
    if(colourState!=ReadState) Transition(list,colour,colourState,ReadState);
    Transition(list,in.depth,WriteState,ReadState); Transition(list,in.motion,WriteState,ReadState);
    if(pad.Enabled()) pad.Fill(d);
    currentFrame={}; auto &f=currentFrame;
    f.colour=colour; f.depth=in.depth; f.motion=in.motion; f.output=output.Get();
    f.colourRect=colourRect; f.guideRect=guideRect; f.outputRect=outputRect;
    f.mvScaleX=in.mvScaleX; f.mvScaleY=in.mvScaleY; f.scalingRatio=create.scalingRatio;
    f.depthInverted=depthInverted; f.reset=in.reset || pendingReset;
    NVSDK_NGX_Result result=NVSDK_NGX_Result_Success;
    if(useCore) {
        auto resource=[](ID3D12Resource *r, const NrRect &rect, D3D12_RESOURCE_STATES state) {
            return OfpsResource{sizeof(OfpsResource),r,r ? r->GetDesc().Format : DXGI_FORMAT_UNKNOWN,
                {rect.x,rect.y,rect.width,rect.height},state,0};
        };
        OfpsFrameInputs inputs{}; inputs.size=sizeof(inputs);
        inputs.color=resource(f.colour,f.colourRect,ReadState);
        inputs.depth=resource(f.depth,f.guideRect,ReadState);
        inputs.motion=resource(f.motion,f.guideRect,ReadState);
        inputs.output=resource(f.output,f.outputRect,WriteState);
        inputs.mvScaleX=f.mvScaleX; inputs.mvScaleY=f.mvScaleY;
        inputs.depthInverted=f.depthInverted; inputs.hostReset=f.reset;
        inputs.colorDomain=OFPS_COLOR_DISPLAY_REFERRED;
        const int rc=coreSession.Evaluate(list,inputs);
        result=rc>=0 ? NVSDK_NGX_Result_Success : NVSDK_NGX_Result_FAIL_InvalidParameter;
        if(rc<0) std::fprintf(stderr,"[core host] evaluate %d, model NGX 0x%08X\n",rc,unsigned(coreSession.model.last));
    } else {
        parameters.Audit(calls==0); NrWriteEvaluate(parameters,f); parameters.Audit(false);
        result=runtime.Evaluate(list,feature,&parameters);
    }
    // The runtime binds its own descriptor heap; the region decode below needs the bench's again.
    ID3D12DescriptorHeap *heaps[]={d.heap.Get()}; list->SetDescriptorHeaps(1,heaps);
    if(colourState!=ReadState) Transition(list,colour,ReadState,colourState);
    Transition(list,in.depth,ReadState,WriteState); Transition(list,in.motion,ReadState,WriteState);
    const bool report=calls<3 || frame%60==0 || result!=lastResult;
    if(pad.Enabled() && report) pad.Capture(d,frame);
    presented=NVSDK_NGX_SUCCEED(result);
    if(presented) bridge.Resolve(d,outputRect.x,outputRect.y);
    ++calls; pendingReset=false;
    if(presented) ++succeeded; else ++failed;
    if(report) {
        char where[96]="";
        if(pad.Enabled()) std::snprintf(where,sizeof(where)," at %u,%u in a %llux%u texture",outputRect.x,outputRect.y,
            output->GetDesc().Width,output->GetDesc().Height);
        std::printf("[nr] frame %d: mode %s, feature %ux%u, colour %ux%u, output %ux%u%s, result 0x%08X%s\n",frame,mode.c_str(),
            create.width,create.height,colourRect.width,colourRect.height,outputRect.width,outputRect.height,where,unsigned(result),
            presented ? "" : " (evaluate refused; the input colour is presented)");
    }
    lastResult=result;
    runtime.log.Echo();
}
void NeuralRendering::Release(Device &d)
{
    presented=false;
    if(useCore) {
        d.Wait();
        if(!coreSession.Close()) throw std::runtime_error("core session drain timed out");
        d.onSubmitted=nullptr; d.submissionContext=nullptr;
    }
    if(feature) {
        d.Wait();
        const auto result=runtime.Release(feature);
        if(NVSDK_NGX_FAILED(result)) std::fprintf(stderr,"[nr] release feature: 0x%08X\n",unsigned(result));
        feature=nullptr;
    }
    pad.Reset(); bridge.Reset(); output.Reset();
}
void NeuralRendering::Shutdown(Device &d)
{
    if(mode=="off") return;
    d.Wait(); pad.Report();
    Release(d);
    runtime.log.Echo();
    std::printf("[nr] evaluates: %u succeeded, %u refused, last result 0x%08X\n",succeeded,failed,unsigned(lastResult));
    runtime.log.Summary();
    mode="off";
}
