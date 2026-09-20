#pragma once

#include <d3d12.h>
#include <cstdint>

namespace pwtemporal {
struct Resources;
struct FrameInputs;
struct SlotKey;
struct Constants;

// Two preceding full passes. Pictures retain detail at 1/3 per axis; geometry is 1/6.
class History {
public:
    ~History();
    bool Create(ID3D12Device *device, std::uint32_t width, std::uint32_t height);
    void Reset() { count_ = 0; }
    void Store(Resources &resources, ID3D12GraphicsCommandList *cmd, const FrameInputs &in,
               const SlotKey &previous, bool connected);
    void Bind(Resources &resources, ID3D12GraphicsCommandList *cmd, SlotKey &key, Constants &constants);

private:
    struct Pass {
        ID3D12Resource *textures[4]{}; // residual, colour, depth, link
        D3D12_RESOURCE_STATES states[4]{};
    } passes_[2];
    std::uint32_t pictureW_ = 0, pictureH_ = 0, geometryW_ = 0, geometryH_ = 0;
    std::uint32_t newest_ = 0, count_ = 0;
};
} // namespace pwtemporal
