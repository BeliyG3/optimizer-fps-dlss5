#include "core/frame/callback_guard.h"
#include "core/frame/codec_frame.h"
#include "core/frame/model_protocol.h"
#include "core/frame/feature_state.h"
#include "core/context.h"

namespace ofps::core {
namespace {

bool EnsureTexture(FeatureState &st, ID3D12Resource *&texture, D3D12_RESOURCE_STATES &state,
                   const D3D12_RESOURCE_DESC &desc)
{
    if (texture) {
        const auto current = texture->GetDesc();
        if (current.Width == desc.Width && current.Height == desc.Height && current.Format == desc.Format &&
            current.DepthOrArraySize == desc.DepthOrArraySize && current.MipLevels == desc.MipLevels &&
            current.Flags == desc.Flags && current.SampleDesc.Count == desc.SampleDesc.Count) return true;
    }
    ID3D12Resource *next = nullptr;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    ID3D12Device *device = st.realDevice ? st.realDevice : st.device;
    if (!device || FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&next)))) return false;
    if (texture) {
        gpu::Grave grave;
        grave.modelHost = st.modelHost;
        grave.gate = RetirementGate(st);
        grave.objects.push_back(texture);
        Ctx().graveyard.Add(std::move(grave), Ctx().evalCounter);
    }
    texture = next;
    state = D3D12_RESOURCE_STATE_COMMON;
    return true;
}

int Snapshot(FeatureState &st, ID3D12GraphicsCommandList *cmd, const OfpsResource &source, OfpsResource &out)
{
    auto desc = source.res->GetDesc();
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    if (!EnsureTexture(st, st.frameSnapshot, st.frameSnapshotState, desc)) return OFPS_E_DEVICE;
    BarrierExternal(cmd, source.res, source.restState, D3D12_RESOURCE_STATE_COPY_SOURCE, source.subresource);
    Barrier(cmd, st.frameSnapshot, st.frameSnapshotState, D3D12_RESOURCE_STATE_COPY_DEST);
    if (source.subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES) {
        cmd->CopyResource(st.frameSnapshot, source.res);
    } else {
        D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
        dst.pResource = st.frameSnapshot; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.pResource = source.res; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = source.subresource;
        cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    Barrier(cmd, st.frameSnapshot, st.frameSnapshotState, kModelInputState);
    BarrierExternal(cmd, source.res, D3D12_RESOURCE_STATE_COPY_SOURCE, source.restState, source.subresource);
    out = source;
    out.res = st.frameSnapshot;
    if (out.view == DXGI_FORMAT_UNKNOWN) out.view = TypedView(desc.Format, false);
    out.restState = st.frameSnapshotState;
    out.subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    return OFPS_OK;
}

} // namespace

int SnapshotFrameColor(FeatureState &st, ID3D12GraphicsCommandList *cmd,
                       const OfpsResource &source, OfpsResource &copy) {
    if (!cmd || !source.res) return OFPS_E_ARG;
    return Snapshot(st, cmd, source, copy);
}

int BeginCodecFrame(FeatureState &st, ID3D12GraphicsCommandList *cmd,
                    const OfpsFrameInputs &frame, CodecFrame &codec)
{
    codec = {};
    codec.frame = frame;
    if (!st.modelHost || !frame.color.res || !frame.output.res) return OFPS_E_ARG;
    if (!st.device && (FAILED(cmd->GetDevice(IID_PPV_ARGS(&st.device))) || !st.device)) return OFPS_E_DEVICE;
    if (!st.realDevice && (FAILED(frame.color.res->GetDevice(IID_PPV_ARGS(&st.realDevice))) || !st.realDevice)) return OFPS_E_DEVICE;
    OfpsResource snapshot{};
    if (frame.color.res == frame.output.res) {
        const int result = Snapshot(st, cmd, frame.color, snapshot);
        if (result != OFPS_OK) return result;
    }
    const int result = PrepareCodec(st, cmd, frame, &codec.modelColor, &codec.frameBefore, &codec.identity);
    if (result != OFPS_OK) return result < OFPS_OK ? result : OFPS_E_STATE;
    if (codec.modelColor.view == DXGI_FORMAT_UNKNOWN)
        codec.modelColor.view = TypedView(codec.modelColor.res->GetDesc().Format, false);
    if (codec.frameBefore.res && codec.frameBefore.view == DXGI_FORMAT_UNKNOWN)
        codec.frameBefore.view = TypedView(codec.frameBefore.res->GetDesc().Format, false);
    codec.answer = frame.output;
    if (codec.identity) {
        if (snapshot.res) codec.frameBefore = snapshot;
        return OFPS_OK;
    }
    if (!codec.frameBefore.res) {
        if (!snapshot.res) {
            const int copied = Snapshot(st, cmd, frame.color, snapshot);
            if (copied != OFPS_OK) return copied;
        }
        codec.frameBefore = snapshot;
    }
    auto desc = codec.modelColor.res->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.SampleDesc.Count != 1) return OFPS_E_ARG;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    if (!EnsureTexture(st, st.answer, st.answerState, desc)) return OFPS_E_DEVICE;
    Barrier(cmd, st.answer, st.answerState, kModelOutputState);
    codec.answer = codec.modelColor;
    codec.answer.res = st.answer;
    codec.answer.restState = st.answerState;
    codec.answer.subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    return OFPS_OK;
}

int ResolveCodecFrame(FeatureState &st, ID3D12GraphicsCommandList *cmd, CodecFrame &codec)
{
    if (codec.identity) return OFPS_OK;
    const int result = GuardCallback([&] { return st.modelHost->ResolveAnswer(cmd, &codec.answer, &codec.frame); });
    if (result != OFPS_S_IDENTITY) return result <= OFPS_OK ? result : OFPS_E_STATE;
    const auto &src = codec.answer;
    const auto &dst = codec.frame.output;
    if (src.rect.w != dst.rect.w || src.rect.h != dst.rect.h ||
        src.res->GetDesc().Format != dst.res->GetDesc().Format) return OFPS_E_ARG;
    BarrierExternal(cmd, src.res, src.restState, D3D12_RESOURCE_STATE_COPY_SOURCE, src.subresource);
    BarrierExternal(cmd, dst.res, dst.restState, D3D12_RESOURCE_STATE_COPY_DEST, dst.subresource);
    D3D12_TEXTURE_COPY_LOCATION from{}, to{};
    from.pResource = src.res; from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    to.pResource = dst.res; to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    from.SubresourceIndex = src.subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES ? 0 : src.subresource;
    to.SubresourceIndex = dst.subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES ? 0 : dst.subresource;
    const D3D12_BOX box{src.rect.x, src.rect.y, 0, src.rect.x + src.rect.w, src.rect.y + src.rect.h, 1};
    cmd->CopyTextureRegion(&to, dst.rect.x, dst.rect.y, 0, &from, &box);
    BarrierExternal(cmd, dst.res, D3D12_RESOURCE_STATE_COPY_DEST, dst.restState, dst.subresource);
    BarrierExternal(cmd, src.res, D3D12_RESOURCE_STATE_COPY_SOURCE, src.restState, src.subresource);
    return OFPS_OK;
}
} // namespace ofps::core
