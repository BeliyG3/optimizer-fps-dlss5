#include "timing.h"

#include "hook_context.h"

#include <cstring>

namespace pwhook {

// ---------------------------------------------------------------------------------------------
// Diagnostics for the temporal modes (stage 24): model timing.
// ---------------------------------------------------------------------------------------------
bool EnsureTiming(FeatureState &st, ID3D12GraphicsCommandList *cmd)
{
    if (st.timingHeap != nullptr) return true;
    ID3D12Device *device = st.device;
    ID3D12Device *owned = nullptr;
    if (device == nullptr) {
        if (FAILED(cmd->GetDevice(IID_PPV_ARGS(&owned))) || owned == nullptr) return false;
        device = owned;
    }
    D3D12_QUERY_HEAP_DESC heapDesc{};
    heapDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    heapDesc.Count = FeatureState::kTimingSlots * 2;
    bool ok = SUCCEEDED(device->CreateQueryHeap(&heapDesc, IID_PPV_ARGS(&st.timingHeap)));
    if (ok) {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = FeatureState::kTimingSlots * 2 * sizeof(UINT64);
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ok = SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                      IID_PPV_ARGS(&st.timingReadback)));
    }
    if (ok && st.timingFrequency == 0) {
        ID3D12Device *real = st.realDevice;
        ok = pwngx::TimestampFrequency(real, device, &st.timingFrequency) && st.timingFrequency != 0;
        if (!ok) Log(true, "Optimizer FPS NGX hook: timing: no registered queue to take the timestamp frequency from");
    }
    if (owned) owned->Release();
    if (!ok) {
        if (st.timingHeap) { st.timingHeap->Release(); st.timingHeap = nullptr; }
        if (st.timingReadback) { st.timingReadback->Release(); st.timingReadback = nullptr; }
        Ctx().diag.timing = false; // could not be set up: stop trying
        Log(true, "Optimizer FPS NGX hook: timing disabled (query objects could not be created)");
    }
    return ok;
}

// Reads the slot that is about to be reused (written a full ring of evaluates ago), then begins a
// new pair in it.
void TimingBegin(FeatureState &st, ID3D12GraphicsCommandList *cmd)
{
    if (!TimingEnabled() || !EnsureTiming(st, cmd)) return;
    const std::uint32_t slot = static_cast<std::uint32_t>(Ctx().evalCounter % FeatureState::kTimingSlots);
    if (st.timingEval[slot] != 0 && Ctx().evalCounter - st.timingEval[slot] >= 16) {
        const D3D12_RANGE range{slot * 2 * sizeof(UINT64), (slot * 2 + 2) * sizeof(UINT64)};
        void *mapped = nullptr;
        if (SUCCEEDED(st.timingReadback->Map(0, &range, &mapped)) && mapped) {
            UINT64 stamps[2];
            std::memcpy(stamps, static_cast<char *>(mapped) + range.Begin, sizeof(stamps));
            const D3D12_RANGE none{0, 0};
            st.timingReadback->Unmap(0, &none);
            if (stamps[1] > stamps[0]) {
                const double ms = 1000.0 * static_cast<double>(stamps[1] - stamps[0]) / static_cast<double>(st.timingFrequency);
                st.timingSumFull += ms;
                ++st.timingCountFull;
                Ctx().status.modelMsFull = st.timingCountFull ? static_cast<float>(st.timingSumFull / st.timingCountFull) : 0.0f;
                Ctx().status.modelSamplesFull = st.timingCountFull;
                if (st.timingCountFull % 120 == 0)
                    Log(false, "Optimizer FPS NGX hook: model time: %.3f ms (n=%u)", Ctx().status.modelMsFull, st.timingCountFull);
            }
        }
    }
    st.timingEval[slot] = Ctx().evalCounter;
    st.timingCurrent = slot;
    cmd->EndQuery(st.timingHeap, D3D12_QUERY_TYPE_TIMESTAMP, slot * 2);
}

void TimingEnd(FeatureState &st, ID3D12GraphicsCommandList *cmd)
{
    if (st.timingCurrent >= FeatureState::kTimingSlots || st.timingHeap == nullptr) return;
    const std::uint32_t slot = st.timingCurrent;
    st.timingCurrent = FeatureState::kTimingSlots;
    cmd->EndQuery(st.timingHeap, D3D12_QUERY_TYPE_TIMESTAMP, slot * 2 + 1);
    cmd->ResolveQueryData(st.timingHeap, D3D12_QUERY_TYPE_TIMESTAMP, slot * 2, 2, st.timingReadback, slot * 2 * sizeof(UINT64));
}

} // namespace pwhook
