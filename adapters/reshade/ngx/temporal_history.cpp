#include "temporal_history.h"
#include "temporal_resources.h"

#include <algorithm>

namespace pwtemporal {
namespace {
constexpr DXGI_FORMAT kFormats[] = {DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT,
                                   DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT};
}

History::~History()
{
    for (auto &pass : passes_)
        for (auto *texture : pass.textures)
            if (texture) texture->Release();
}

bool History::Create(ID3D12Device *device, std::uint32_t width, std::uint32_t height)
{
    pictureW_ = (width + 2) / 3; pictureH_ = (height + 2) / 3;
    geometryW_ = (width + 5) / 6; geometryH_ = (height + 5) / 6;
    for (auto &pass : passes_)
        for (unsigned i = 0; i < 4; ++i)
            if (!pwngx::CreateTexture(device, i < 2 ? pictureW_ : geometryW_, i < 2 ? pictureH_ : geometryH_,
                                     kFormats[i], D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, &pass.textures[i]))
                return false;
    return true;
}

void History::Store(Resources &m, ID3D12GraphicsCommandList *cmd, const FrameInputs &in,
                    const SlotKey &previous, bool connected)
{
    if (!connected || !m.colorF) { Reset(); return; }
    const auto slot = (newest_ + 1) % 2;
    auto &pass = passes_[slot];
    for (unsigned i = 0; i < 4; ++i)
        pwngx::Barrier(cmd, pass.textures[i], pass.states[i], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Constants c = m.BaseConstants(in);
    c.fill[1] = static_cast<float>(pictureW_); c.fill[2] = static_cast<float>(pictureH_);
    c.params[0] = 0.0f;
    SlotKey picture = previous;
    picture.res[0] = m.colorF;
    picture.res[2] = m.residualPrev;
    const Resources::Output colour{pass.textures[1], kFormats[1]};
    m.Dispatch(cmd, kHistory, {pass.textures[0], kFormats[0]}, pictureW_, pictureH_, picture, c, &colour);
    c.params[0] = 1.0f;
    const Resources::Output link{pass.textures[3], kFormats[3]};
    m.Dispatch(cmd, kHistory, {pass.textures[2], kFormats[2]}, geometryW_, geometryH_, previous, c, &link);
    for (unsigned i = 0; i < 4; ++i)
        pwngx::Barrier(cmd, pass.textures[i], pass.states[i], kReadable);
    newest_ = slot;
    count_ = (std::min)(count_ + 1, 2u);
}

void History::Bind(Resources &, ID3D12GraphicsCommandList *cmd, SlotKey &key, Constants &constants)
{
    constants.params[0] = static_cast<float>(count_);
    for (unsigned level = 0; level < count_; ++level) {
        auto &pass = passes_[(newest_ + 2 - level) % 2];
        for (unsigned i = 0; i < 4; ++i) {
            pwngx::Barrier(cmd, pass.textures[i], pass.states[i], kReadable);
            key.res[12 + i * 2 + level] = pass.textures[i];
            key.fmt[12 + i * 2 + level] = kFormats[i];
        }
    }
}
} // namespace pwtemporal
