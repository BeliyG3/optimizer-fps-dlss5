#pragma once

#include "core/api/ofps_core.h"
#include "core/gpu/queues.h"
#include "core/shaders/temporal_layout.h"

#include <array>
#include <cstdint>
#include <wrl/client.h>

namespace ofps::core::temporal {

struct PassTimingSnapshot {
    const char* reason = "timing awaiting GPU readback";
    std::array<float, pwtemporalcontract::kPassCount> milliseconds{};
    std::array<std::uint32_t, pwtemporalcontract::kPassCount> samples{};
    std::uint64_t skipped = 0;
};

class PassTimer {
public:
    static constexpr std::uint32_t SlotCount = 256;
    void Enable(bool enabled) noexcept { enabled_ = enabled; }
    bool Enabled() const noexcept { return enabled_; }
    int Begin(ID3D12Device* device, ID3D12GraphicsCommandList* cmd,
              const OfpsFencePoint& point, pwtemporalcontract::Pass pass);
    void End(ID3D12GraphicsCommandList* cmd, const OfpsFencePoint& point, int slot);
    PassTimingSnapshot Snapshot() const { return snapshot_; }
private:
    struct Slot {
        gpu::GateSet gate;
        pwtemporalcontract::Pass pass = pwtemporalcontract::kResidual;
        bool busy = false;
    };
    bool Ensure(ID3D12Device* device);
    void Poll(Slot& slot, std::uint32_t index);
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> queries_;
    Microsoft::WRL::ComPtr<ID3D12Resource> readback_;
    std::array<Slot, SlotCount> slots_{};
    std::uint32_t next_ = 0;
    std::uint64_t frequency_ = 0;
    bool enabled_ = false;
    PassTimingSnapshot snapshot_{};
};

} // namespace ofps::core::temporal
