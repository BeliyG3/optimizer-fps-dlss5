#include "pass_timing.h"

#include <cstring>

namespace ofps::core::temporal {

bool PassTimer::Ensure(ID3D12Device* device) {
    if (queries_) return true;
    if (!gpu::TimestampFrequency(device, device, &frequency_) || frequency_ == 0) {
        snapshot_.reason = "pass timing unavailable: host queue frequency missing";
        return false;
    }
    D3D12_QUERY_HEAP_DESC query{};
    query.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    query.Count = SlotCount * 2;
    if (FAILED(device->CreateQueryHeap(&query, IID_PPV_ARGS(&queries_)))) {
        snapshot_.reason = "pass timing query allocation failed";
        return false;
    }
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = SlotCount * 2 * sizeof(std::uint64_t);
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback_)))) {
        queries_.Reset();
        snapshot_.reason = "pass timing readback allocation failed";
        return false;
    }
    return true;
}

void PassTimer::Poll(Slot& slot, std::uint32_t index) {
    if (!slot.busy || !slot.gate.Completed()) return;
    const std::uint64_t offset = index * 2ull * sizeof(std::uint64_t);
    const D3D12_RANGE range{static_cast<SIZE_T>(offset),
                            static_cast<SIZE_T>(offset + 2 * sizeof(std::uint64_t))};
    void* data = nullptr;
    if (SUCCEEDED(readback_->Map(0, &range, &data)) && data) {
        std::uint64_t stamps[2]{};
        std::memcpy(stamps, static_cast<const char*>(data) + offset, sizeof(stamps));
        const D3D12_RANGE written{0, 0};
        readback_->Unmap(0, &written);
        if (stamps[1] > stamps[0]) {
            const auto p = static_cast<unsigned>(slot.pass);
            const auto n = ++snapshot_.samples[p];
            const float ms = static_cast<float>(1000.0 * static_cast<double>(stamps[1] - stamps[0]) /
                                                static_cast<double>(frequency_));
            snapshot_.milliseconds[p] += (ms - snapshot_.milliseconds[p]) / n;
            snapshot_.reason = nullptr;
        }
    } else {
        snapshot_.reason = "pass timing readback map failed";
    }
    slot.gate.Clear();
    slot.busy = false;
}

int PassTimer::Begin(ID3D12Device* device, ID3D12GraphicsCommandList* cmd,
                     const OfpsFencePoint& point, pwtemporalcontract::Pass pass) {
    if (!enabled_) return -1;
    if (!point.fence || point.value == 0) {
        snapshot_.reason = "pass timing unavailable: submission fence missing";
        return -1;
    }
    if (!Ensure(device)) return -1;
    const auto index = next_;
    Slot& slot = slots_[index];
    Poll(slot, index);
    if (slot.busy) { ++snapshot_.skipped; return -1; }
    next_ = (next_ + 1) % SlotCount;
    slot.pass = pass;
    cmd->EndQuery(queries_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, index * 2);
    return static_cast<int>(index);
}

void PassTimer::End(ID3D12GraphicsCommandList* cmd, const OfpsFencePoint& point, int index) {
    if (index < 0) return;
    const auto slotIndex = static_cast<std::uint32_t>(index);
    cmd->EndQuery(queries_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slotIndex * 2 + 1);
    cmd->ResolveQueryData(queries_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slotIndex * 2, 2,
                          readback_.Get(), slotIndex * 2 * sizeof(std::uint64_t));
    Slot& slot = slots_[slotIndex];
    slot.gate.Add(point.fence, point.value);
    slot.busy = true;
}

} // namespace ofps::core::temporal
