#pragma once

#include <d3d12.h>
#include <cstdint>

namespace ofps::core::temporal {
struct Resources;
struct FrameInputs;
struct SlotKey;
struct Constants;

// The sky field (TemporalSkyField, shaders/temporal_sky.hlsli): the residual averaged over the far-plane pixels of its
// pass, on the low-res grid, filled by push-pull. Carried frames give it, at each pixel's own screen position, to the
// pixels whose current depth is the far plane, instead of the residual carried along a chain that a sky pixel does not
// really have; a publish frame's phase-in leaves those pixels the new residual alone. A carried frame weighs the field
// by how much of its own sky the pass saw (fix round 2): it measures its share of sky on the same lattice, and where
// the pass saw much less (a reveal) the ordinary carry stays.
//
// A pyramid from the low-res grid (lowW x lowH) down to 1x1, every level three times: as pulled (the far-plane mean
// and its coverage), as filled (alpha: the 1x1 level's coverage, the pass's sky support) and as now (the carried
// frame's own coverage). The coarsest level is filled as it is; the finest filled level is the field. A build is
// 1 + 2 x (levels - 1) dispatches (17 at 4K: 240x135 .. 1x1), once per residual; a measure 1 + (levels - 1) (9 at 4K),
// once per carried frame that uses the field; both from each machine's own descriptor ring.
class SkyField {
public:
    ~SkyField();
    // False (and nothing allocated) when a level cannot be created; the caller then leaves the field off.
    bool Create(ID3D12Device *device, std::uint32_t lowWidth, std::uint32_t lowHeight);
    bool Created() const { return count_ != 0; }
    // The field no longer belongs to the residual in place (a reset, a residual recorded without it).
    void Reset() { built_ = false; }
    // From the residual and the residual frame's depth snapshot (m.residual and m.depthF, both readable).
    void Build(Resources &resources, ID3D12GraphicsCommandList *cmd, const FrameInputs &in);
    // A carried frame's share of sky from its own depth (in.depth, readable), on the field's lattice, up to 1x1. Only
    // when the field is built for the residual in place; true when recorded, and only then may the frame passes bind it.
    bool Measure(Resources &resources, ID3D12GraphicsCommandList *cmd, const FrameInputs &in);
    // The field at t20 (PwTSkyField) and the last measured share at t21 (PwTSkyNow) for a pass that reads them,
    // readable, when the field was built for the residual in place and `on`; otherwise null views there, which the
    // shaders read as no support (nothing changes). A pass that reads t21 binds with `on` only after Measure.
    void Bind(ID3D12GraphicsCommandList *cmd, SlotKey &key, bool on);
    static constexpr int kSlot = 20, kNowSlot = 21;

private:
    static constexpr unsigned kMaxLevels = 16;
    struct Level {
        ID3D12Resource *pulled = nullptr, *filled = nullptr, *now = nullptr; // filled: none on the coarsest level
        D3D12_RESOURCE_STATES pulledState{}, filledState{}, nowState{};
        std::uint32_t width = 0, height = 0;
    };
    using Texture = ID3D12Resource *Level::*;
    using State = D3D12_RESOURCE_STATES Level::*;
    // The level's filled texture and its state: the pulled one on the coarsest level.
    ID3D12Resource *Filled(unsigned level) const;
    D3D12_RESOURCE_STATES &FilledState(unsigned level);
    bool CreateLevels(ID3D12Device *device, std::uint32_t lowWidth, std::uint32_t lowHeight);
    // CSSkyPull from the finest level of `texture` up to 1x1, each level from the finer one.
    void PullUp(Resources &resources, ID3D12GraphicsCommandList *cmd, const Constants &c, Texture texture, State state);
    void Release();
    Level levels_[kMaxLevels]{};
    unsigned count_ = 0;
    bool built_ = false;
};
} // namespace ofps::core::temporal
