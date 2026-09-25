#include "core/temporal/async_frame.h"
#include "core/context.h"

namespace ofps::core {

// The host may recycle codec scratch after EndFrame. Keep the residual's base in the
// fenced background job until adoption, independently of the feature's next snapshot.
int SnapshotAsyncCodecBase(FeatureState &st, ID3D12GraphicsCommandList *cmd,
                           const OfpsResource &source, int slot)
{
    AsyncJob &job = *st.async;
    auto desc = source.res->GetDesc();
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) return OFPS_E_ARG;
    ID3D12Resource *&target = job.codecBase[slot];
    auto &state = job.codecBaseState[slot];
    bool matches = false;
    if (target) {
        const auto current = target->GetDesc();
        matches = current.Width == desc.Width && current.Height == desc.Height && current.Format == desc.Format &&
            current.MipLevels == desc.MipLevels && current.DepthOrArraySize == desc.DepthOrArraySize &&
            current.SampleDesc.Count == desc.SampleDesc.Count && current.SampleDesc.Quality == desc.SampleDesc.Quality;
    }
    if (!matches) {
        ID3D12Resource *next = nullptr;
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        if (FAILED(job.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&next)))) return OFPS_E_DEVICE;
        if (target) {
            gpu::Grave grave;
            grave.modelHost = st.modelHost;
        grave.gate = RetirementGate(st);
            grave.objects.push_back(target);
            Ctx().graveyard.Add(std::move(grave), Ctx().evalCounter);
        }
        target = next;
        state = D3D12_RESOURCE_STATE_COMMON;
    }
    BarrierExternal(cmd, source.res, source.restState, D3D12_RESOURCE_STATE_COPY_SOURCE, source.subresource);
    Barrier(cmd, target, state, D3D12_RESOURCE_STATE_COPY_DEST);
    if (source.subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES) {
        cmd->CopyResource(target, source.res);
    } else {
        D3D12_TEXTURE_COPY_LOCATION from{}, to{};
        from.pResource = source.res; from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        from.SubresourceIndex = source.subresource;
        to.pResource = target; to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        cmd->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    }
    Barrier(cmd, target, state, kModelInputState);
    BarrierExternal(cmd, source.res, D3D12_RESOURCE_STATE_COPY_SOURCE, source.restState, source.subresource);
    job.codecBefore[slot] = source;
    job.codecBefore[slot].res = target;
    job.codecBefore[slot].restState = state;
    job.codecBefore[slot].subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    return OFPS_OK;
}
} // namespace ofps::core
