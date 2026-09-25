#include "stats.h"

#include "core/gpu/barriers.h"
#include "core/temporal/resources.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace ofps::core::temporal {
namespace {
constexpr DXGI_FORMAT kFormat = DXGI_FORMAT_R32G32B32A32_FLOAT;
constexpr std::uint32_t kBytes = StatsReader::RowPitch * StatsReader::Height;

void Accumulate(AcceptanceRegion& region, const float* pixel) {
    region.weight += pixel[1];
    region.rejected += pixel[1] < 0.5f ? 1.0f : 0.0f;
    region.noRing += pixel[2];
    region.noFill += pixel[3];
}

void Divide(AcceptanceRegion& region, float n) {
    region.weight /= n;
    region.rejected /= n;
    region.noRing /= n;
    region.noFill /= n;
}

void Add(AcceptanceRegion& total, const AcceptanceRegion& sample) {
    total.weight += sample.weight;
    total.rejected += sample.rejected;
    total.noRing += sample.noRing;
    total.noFill += sample.noFill;
}
} // namespace

StatsReader::~StatsReader() = default;

bool StatsReader::Ensure(ID3D12Device* device) {
    if (texture_) return true;
    ID3D12Resource* texture = nullptr;
    if (!gpu::CreateTexture(device, Width, Height, kFormat,
                            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, &texture)) {
        snapshot_.reason = "stats texture allocation failed";
        return false;
    }
    texture_.Attach(texture);
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = kBytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    for (auto& slot : slots_) {
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&slot.readback)))) {
            texture_.Reset();
            for (auto& old : slots_) old.readback.Reset();
            snapshot_.reason = "stats readback allocation failed";
            return false;
        }
    }
    return true;
}

void StatsReader::Poll() {
    for (;;) {
        Slot* oldest = nullptr;
        for (auto& slot : slots_)
            if (slot.busy && (!oldest || slot.serial < oldest->serial)) oldest = &slot;
        if (!oldest || !oldest->gate.Completed()) return;
        Consume(*oldest);
        oldest->busy = false;
        oldest->gate.Clear();
    }
}

void StatsReader::Consume(Slot& slot) {
    void* mapped = nullptr;
    const D3D12_RANGE range{0, kBytes};
    if (FAILED(slot.readback->Map(0, &range, &mapped)) || !mapped) {
        snapshot_.reason = "stats readback map failed";
        return;
    }
    const float* data = static_cast<const float*>(mapped);
    AcceptanceRegion all{}, centre{}, edges{};
    std::array<GuideRange, 12> guides{};
    for (auto& guide : guides) {
        guide.low = std::numeric_limits<float>::max();
        guide.high = std::numeric_limits<float>::lowest();
    }
    std::uint32_t centreCount = 0;
    for (std::uint32_t y = 0; y < Height; ++y) {
        for (std::uint32_t x = 0; x < GridWidth; ++x) {
            const float* p = data + (y * Width + x) * 4;
            const bool inCentre = x >= GridWidth / 4 && x < GridWidth * 3 / 4 &&
                                  (y + 0.5f) >= Height * 0.25f && (y + 0.5f) < Height * 0.75f;
            Accumulate(all, p);
            Accumulate(inCentre ? centre : edges, p);
            centreCount += inCentre ? 1u : 0u;
            for (std::uint32_t tile = 1; tile < 4; ++tile)
                for (std::uint32_t channel = 0; channel < 4; ++channel) {
                    auto& guide = guides[(tile - 1) * 4 + channel];
                    const float v = data[(y * Width + tile * GridWidth + x) * 4 + channel];
                    if (!std::isfinite(v)) { ++guide.invalid; continue; }
                    guide.low = (std::min)(guide.low, v);
                    guide.high = (std::max)(guide.high, v);
                    guide.mean += v;
                    ++guide.finite;
                }
        }
    }
    const D3D12_RANGE written{0, 0};
    slot.readback->Unmap(0, &written);
    Divide(all, static_cast<float>(GridWidth * Height));
    Divide(centre, static_cast<float>(centreCount));
    Divide(edges, static_cast<float>(GridWidth * Height - centreCount));
    for (auto& guide : guides) {
        if (guide.finite) guide.mean /= static_cast<float>(guide.finite);
        else guide.low = guide.high = 0.0f;
    }
    snapshot_.reason = nullptr;
    if (snapshot_.samples && snapshot_.fillFloor != slot.floor) {
        windowAll_ = {}; windowCentre_ = {}; windowEdges_ = {};
        windowFrames_ = 0;
    }
    Add(windowAll_, all);
    Add(windowCentre_, centre);
    Add(windowEdges_, edges);
    ++windowFrames_;
    snapshot_.windowAll = windowAll_;
    snapshot_.windowCentre = windowCentre_;
    snapshot_.windowEdges = windowEdges_;
    Divide(snapshot_.windowAll, static_cast<float>(windowFrames_));
    Divide(snapshot_.windowCentre, static_cast<float>(windowFrames_));
    Divide(snapshot_.windowEdges, static_cast<float>(windowFrames_));
    snapshot_.windowFrames = windowFrames_;
    snapshot_.all = all;
    snapshot_.centre = centre;
    snapshot_.edges = edges;
    snapshot_.guides = guides;
    snapshot_.fillFloor = slot.floor;
    ++snapshot_.samples;
    if (windowFrames_ == 120) {
        windowAll_ = {}; windowCentre_ = {}; windowEdges_ = {};
        windowFrames_ = 0;
    }
}

void StatsReader::Record(Resources& resources, ID3D12GraphicsCommandList* cmd,
                         const SlotKey& key, const Constants& constants, bool floor) {
    Poll();
    if (!resources.usePoint.fence || resources.usePoint.value == 0) {
        snapshot_.reason = "stats readback unavailable: submission fence missing";
        return;
    }
    if (!Ensure(resources.device)) return;
    Slot& slot = slots_[next_];
    if (slot.busy) { ++snapshot_.skipped; return; }
    gpu::Barrier(cmd, texture_.Get(), textureState_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Constants sample = constants;
    sample.params[2] = -1.0f;
    resources.Dispatch(cmd, kStats, {texture_.Get(), kFormat}, Width, Height, key, sample);
    if (resources.exhausted) { ++snapshot_.skipped; return; }
    gpu::Barrier(cmd, texture_.Get(), textureState_, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION source{}, destination{};
    source.pResource = texture_.Get();
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    destination.pResource = slot.readback.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint.Footprint = {kFormat, Width, Height, 1, RowPitch};
    cmd->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    gpu::Barrier(cmd, texture_.Get(), textureState_, kReadable);
    slot.gate.Add(resources.usePoint.fence, resources.usePoint.value);
    slot.busy = true;
    slot.floor = floor;
    slot.serial = ++serial_;
    next_ = (next_ + 1) % slots_.size();
}

} // namespace ofps::core::temporal
