#pragma once

#include "core/gpu/queues.h"
#include "core/api/ofps_core.h"

#include <array>
#include <cstdint>
#include <wrl/client.h>

namespace ofps::core::temporal {

struct Resources;
struct SlotKey;
struct Constants;

struct AcceptanceRegion {
    float weight = 0.0f;
    float rejected = 0.0f;
    float noRing = 0.0f;
    float noFill = 0.0f;
};

struct GuideRange {
    float low = 0.0f;
    float high = 0.0f;
    float mean = 0.0f;
    std::uint32_t finite = 0;
    std::uint32_t invalid = 0;
};

struct StatsSnapshot {
    const char* reason = "awaiting GPU readback";
    std::uint64_t samples = 0;
    std::uint64_t skipped = 0;
    bool fillFloor = false;
    AcceptanceRegion all, centre, edges;
    AcceptanceRegion windowAll, windowCentre, windowEdges;
    std::uint32_t windowFrames = 0;
    std::array<GuideRange, 12> guides{};
};

class StatsReader {
public:
    static constexpr std::uint32_t GridWidth = 32, Width = 128, Height = 18;
    static constexpr std::uint32_t RowPitch = Width * 4 * sizeof(float);
    StatsReader() = default;
    StatsReader(const StatsReader&) = delete;
    StatsReader& operator=(const StatsReader&) = delete;
    ~StatsReader();
    void Record(Resources& resources, ID3D12GraphicsCommandList* cmd,
                const SlotKey& key, const Constants& constants, bool floor);
    StatsSnapshot Snapshot() const { return snapshot_; }
private:
    struct Slot {
        Microsoft::WRL::ComPtr<ID3D12Resource> readback;
        gpu::GateSet gate;
        std::uint64_t serial = 0;
        bool busy = false;
        bool floor = false;
    };
    bool Ensure(ID3D12Device* device);
    void Poll();
    void Consume(Slot& slot);
    Microsoft::WRL::ComPtr<ID3D12Resource> texture_;
    D3D12_RESOURCE_STATES textureState_ = D3D12_RESOURCE_STATE_COMMON;
    std::array<Slot, 8> slots_{};
    std::uint32_t next_ = 0;
    std::uint64_t serial_ = 0;
    StatsSnapshot snapshot_{};
    AcceptanceRegion windowAll_{}, windowCentre_{}, windowEdges_{};
    std::uint32_t windowFrames_ = 0;
};

} // namespace ofps::core::temporal
