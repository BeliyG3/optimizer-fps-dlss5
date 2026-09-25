#include "core/frame/warp_recorder.h"
#include "core/frame/codec_frame.h"
#include "core/context.h"

namespace ofps::core {
int FinishWarpCodec(EvalContext &c)
{
    auto &st = *c.st;
    auto &codec = *c.codec;
    if (st.warpPath == warp::PackPath::Pixel) {
        Barrier(c.cmd, st.unpackTarget, st.unpackState, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Barrier(c.cmd, st.answer, st.answerState, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
        src.pResource = st.unpackTarget; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.pResource = st.answer; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        const D3D12_BOX box{0, 0, 0, st.layout.nativeWidth, st.layout.nativeHeight, 1};
        c.cmd->CopyTextureRegion(&dst, codec.answer.rect.x, codec.answer.rect.y, 0, &src, &box);
        Barrier(c.cmd, st.answer, st.answerState, codec.answer.restState);
    }
    const int resolved = ResolveCodecFrame(st, c.cmd, codec);
    if (resolved != OFPS_OK) return resolved;
    if (c.temporalActive) {
        auto tin = TemporalInputsWithBase(c);
        tin.residualBlend = Ctx().temporal.debugSingleFrameMotion ? 0.0f : kResidualBlend;
        st.temporal->RecordResidual(c.cmd, tin, codec.frame.output.res, codec.frame.output.restState, codec.frame.output.subresource);
        if (st.temporal->PhaseInActive())
            st.temporal->RecordApply(c.cmd, tin, codec.frame.output.res, codec.frame.output.restState,
                codec.frame.output.rect.x, codec.frame.output.rect.y, codec.frame.output.subresource);
    }
    return OFPS_OK;
}
} // namespace ofps::core
