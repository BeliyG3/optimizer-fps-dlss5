#include "core/frame/spread_passes.h"

#include "core/frame/feature_state.h"
#include "core/frame/codec_frame.h"
#include "core/frame/model_grid.h"
#include "core/context.h"
#include "core/frame/model_passes.h"
#include "core/frame/timing.h"
#include "core/frame/warp_recorder.h"

namespace ofps::core {
int SpreadModel(FeatureState &st, ID3D12GraphicsCommandList *cmd, const OfpsModelInputs &inputs, const OfpsFrameInputs &frame,
                const ofps::core::temporal::FrameInputs &in, ID3D12Resource *input, ofps::core::temporal::Machine &stage)
{
    OfpsFrameInputs stageFrame = frame;
    stageFrame.color = inputs.color;
    stageFrame.color.res = input;
    stageFrame.color.view = in.colorView;
    stageFrame.color.rect = {0, 0, st.nativeWidth, st.nativeHeight};
    stageFrame.color.restState = kModelInputState;
    stageFrame.color.subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    CodecFrame codec{};
    const int prepared = BeginCodecFrame(st, cmd, stageFrame, codec);
    if (prepared != OFPS_OK) return prepared;
    const int grid = ConfigureModelGrid(st, cmd, codec);
    if (grid != OFPS_OK) return grid;
    const auto &modelColor = codec.modelColor;
    const auto modelRect = modelColor.rect;
    const auto modelView = modelColor.view != DXGI_FORMAT_UNKNOWN
        ? modelColor.view : TypedView(modelColor.res->GetDesc().Format, false);
    auto *vectors = in.motion;
    bool accumulated = stage.AccValid() && !Ctx().temporal.debugSingleFrameMotion;
    if (accumulated) {
        vectors = stage.Acc();
        if (!Ctx().diag.temporalNoModelMotion && stage.ModelMv()) {
            stage.RecordModelMotion(cmd, in);
            vectors = stage.ModelMv();
        }
    }
    int result = OFPS_OK;
    if (!st.warped) {
        OfpsModelInputs modelInputs = inputs;
        modelInputs.color = modelColor;
        modelInputs.output = codec.answer;
        if (!codec.identity) {
            modelInputs.width = modelRect.w;
            modelInputs.height = modelRect.h;
        }
        modelInputs.motion.res = vectors;
        if (accumulated) {
            modelInputs.mvScaleX = modelInputs.mvScaleY = 1.0f;
            modelInputs.motion.view = DXGI_FORMAT_R16G16_FLOAT;
            modelInputs.motion.restState = kModelInputState;
            modelInputs.motion.subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        }
        TimingBegin(st, cmd);
        result = EvaluateModelPasses(st, cmd, modelInputs);
        TimingEnd(st, cmd);
        if (result == OFPS_OK) result = ResolveCodecFrame(st, cmd, codec);
    } else {
        EvalContext c{};
        c.st = &st; c.cmd = cmd; c.modelInputs = inputs;
        c.frame = &stageFrame; c.codec = &codec;
        c.color = modelColor.res; c.depth = in.depth; c.motion = vectors; c.hostMotion = in.motion;
        c.output = codec.answer.res;
        c.colorResource = codec.modelColor;
        c.motionResource = inputs.motion;
        c.outputResource = codec.answer;
        c.ui = inputs.ui.res;
        c.uiAlpha = inputs.uiAlpha.res;
        c.backbuffer = inputs.backbuffer.res;
        c.colorRect = {modelRect.x, modelRect.y, modelRect.w, modelRect.h};
        c.depthRect = {in.depthRect.x, in.depthRect.y, in.depthRect.w, in.depthRect.h};
        c.motionRect = {in.motionRect.x, in.motionRect.y, in.motionRect.w, in.motionRect.h};
        c.outputRect = {codec.answer.rect.x, codec.answer.rect.y, codec.answer.rect.w, codec.answer.rect.h};
        c.depthState = in.depthState; c.depthSub = in.depthSubresource;
        c.mvScaleX = in.mvScaleX; c.mvScaleY = in.mvScaleY;
        c.motionIsAcc = accumulated;
        c.depthConvention = in.depthInverted ? ofps::sdk::DepthConvention::Reversed : ofps::sdk::DepthConvention::Normal;
        c.slot = c.packSlot = st.nextPackSlot;
        st.nextPackSlot = (st.nextPackSlot + 1) % FeatureState::kBgPackSlot;
        c.packSet = st.warpPath == warp::PackPath::Pixel ?
            st.packSets.Acquire(HostUsePoint(cmd), Ctx().evalCounter, gpu::kPoolWaitMilliseconds) : 0u;
        // SpreadEvaluate owns fallback output and accounts for the failed frame once.
        if (st.warpPath == warp::PackPath::Pixel && c.packSet == gpu::DescriptorPool::kNone) return kPackFailed;
        c.unpackSlot = FeatureState::kPackSlots + c.packSlot;
        auto description = ofps::sdk::DefaultInputDescriptionV2(modelRect.w, modelRect.h);
        description.colorRect = {modelRect.x, modelRect.y, modelRect.w, modelRect.h};
        description.depthRect = {in.depthRect.x, in.depthRect.y, in.depthRect.w, in.depthRect.h};
        description.motionRect = {in.motionRect.x, in.motionRect.y, in.motionRect.w, in.motionRect.h};
        description.confidenceRect = {0, 0, 1, 1};
        const float adjust = Ctx().motionScaleAdjust * (Ctx().motionInvert ? -1.0f : 1.0f);
        description.motionScaleX = adjust * (accumulated ? 1.0f : in.mvScaleX) * static_cast<float>(modelRect.w) / in.motionRect.w;
        description.motionScaleY = adjust * (accumulated ? 1.0f : in.mvScaleY) * static_cast<float>(modelRect.h) / in.motionRect.h;
        description.motionDirection = ofps::sdk::MotionDirection::CurrentToPrevious;
        description.depthConvention = c.depthConvention;
        description.colorEncoding = EncodingFor(modelView);
        const ofps::sdk::D3D12SourceResources sources = {{modelColor.res, modelView}, {in.depth, in.depthView},
            {vectors, accumulated ? DXGI_FORMAT_R16G16_FLOAT : in.motionView}, {nullptr, DXGI_FORMAT_UNKNOWN}};
        c.packSources = sources; c.packInput = description;
        if (st.warpPath == warp::PackPath::Pixel &&
            st.adapter->WriteSourceSetV2(c.packSet, sources, description) != ofps::sdk::AdapterStatus::Ok) return kPackFailed;
        NotifyFirstWarped();
        result = WarpedGuarded(c);
    }
    return result;
}
} // namespace ofps::core
