#include "core/temporal/sky_field.h"
#include "core/temporal/resources.h"

namespace ofps::core::temporal {
namespace {
constexpr DXGI_FORMAT kFieldFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
constexpr D3D12_RESOURCE_STATES kWritable = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
using ofps::core::gpu::Barrier;
} // namespace

SkyField::~SkyField() { Release(); }

void SkyField::Release()
{
    for (auto &level : levels_) {
        for (ID3D12Resource *texture : {level.pulled, level.filled, level.now})
            if (texture) texture->Release();
        level = Level{};
    }
    count_ = 0;
    built_ = false;
}

bool SkyField::Create(ID3D12Device *device, std::uint32_t lowWidth, std::uint32_t lowHeight)
{
    Release();
    if (!CreateLevels(device, lowWidth, lowHeight)) {
        Release(); // nothing half-made stays behind: Created() is false and the field stays off
        return false;
    }
    return true;
}

bool SkyField::CreateLevels(ID3D12Device *device, std::uint32_t lowWidth, std::uint32_t lowHeight)
{
    constexpr D3D12_RESOURCE_FLAGS kTarget = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    std::uint32_t w = lowWidth, h = lowHeight;
    for (unsigned k = 0; k < kMaxLevels && w != 0 && h != 0; ++k) {
        Level &level = levels_[k];
        level.width = w;
        level.height = h;
        const bool coarsest = w == 1 && h == 1;
        if (!ofps::core::gpu::CreateTexture(device, w, h, kFieldFormat, kTarget, &level.pulled) ||
            !ofps::core::gpu::CreateTexture(device, w, h, kFieldFormat, kTarget, &level.now) ||
            (!coarsest && !ofps::core::gpu::CreateTexture(device, w, h, kFieldFormat, kTarget, &level.filled)))
            return false;
        if (coarsest) {
            count_ = k + 1;
            return true;
        }
        w = (w + 1) / 2;
        h = (h + 1) / 2;
    }
    return false; // no 1x1 level within kMaxLevels: not a size the low-res grid has
}

ID3D12Resource *SkyField::Filled(unsigned level) const
{
    return level + 1 == count_ ? levels_[level].pulled : levels_[level].filled;
}

D3D12_RESOURCE_STATES &SkyField::FilledState(unsigned level)
{
    return level + 1 == count_ ? levels_[level].pulledState : levels_[level].filledState;
}

void SkyField::PullUp(Resources &m, ID3D12GraphicsCommandList *cmd, const Constants &c, Texture texture, State state)
{
    // PwTParams.x: the finest-level cells a texel of the finer level stands for per side, so the edge texels of an odd
    // size weigh by what they cover.
    for (unsigned k = 1; k < count_; ++k) {
        Level &level = levels_[k];
        SlotKey pull;
        pull.valid = true;
        pull.res[2] = levels_[k - 1].*texture; pull.fmt[2] = kFieldFormat;
        Constants pc = c;
        pc.params[0] = static_cast<float>(1u << (k - 1));
        Barrier(cmd, level.*texture, level.*state, kWritable);
        m.Dispatch(cmd, kSkyPull, {level.*texture, kFieldFormat}, level.width, level.height, pull, pc);
        Barrier(cmd, level.*texture, level.*state, kReadable);
    }
}

void SkyField::Build(Resources &m, ID3D12GraphicsCommandList *cmd, const FrameInputs &in)
{
    built_ = false;
    if (count_ == 0) return;
    const Constants c = m.BaseConstants(in); // PwTSky.x: the far plane; PwTFill.yz: the low-res grid; PwTDepthRect
    SlotKey down;
    down.valid = true;
    down.res[2] = m.residual; down.fmt[2] = kFieldFormat;
    down.res[6] = m.depthF; down.fmt[6] = in.depthView; // this residual's frame: RecordResidual copied it in
    Level &finest = levels_[0];
    Barrier(cmd, finest.pulled, finest.pulledState, kWritable);
    m.Dispatch(cmd, kSkyDown, {finest.pulled, kFieldFormat}, finest.width, finest.height, down, c);
    Barrier(cmd, finest.pulled, finest.pulledState, kReadable);
    PullUp(m, cmd, c, &Level::pulled, &Level::pulledState);
    // Push: each level filled from the filled coarser one, back down to the finest.
    for (unsigned k = count_ - 1; k-- > 0;) {
        Level &level = levels_[k];
        SlotKey push;
        push.valid = true;
        push.res[1] = Filled(k + 1); push.fmt[1] = kFieldFormat;
        push.res[2] = level.pulled; push.fmt[2] = kFieldFormat;
        Barrier(cmd, level.filled, level.filledState, kWritable);
        m.Dispatch(cmd, kSkyPush, {level.filled, kFieldFormat}, level.width, level.height, push, c);
        Barrier(cmd, level.filled, level.filledState, kReadable);
    }
    built_ = !m.exhausted; // an exhausted descriptor ring skipped dispatches: no field
}

bool SkyField::Measure(Resources &m, ID3D12GraphicsCommandList *cmd, const FrameInputs &in)
{
    if (!built_ || count_ == 0) return false;
    const Constants c = m.BaseConstants(in); // this frame's far plane, depth sub-rect and the low-res grid
    SlotKey down;
    down.valid = true;
    down.res[5] = in.depth; down.fmt[5] = in.depthView;
    Level &finest = levels_[0];
    Barrier(cmd, finest.now, finest.nowState, kWritable);
    m.Dispatch(cmd, kSkyNow, {finest.now, kFieldFormat}, finest.width, finest.height, down, c);
    Barrier(cmd, finest.now, finest.nowState, kReadable);
    PullUp(m, cmd, c, &Level::now, &Level::nowState);
    return !m.exhausted; // an exhausted ring skipped the measure: the frame forms its picture without the field
}

void SkyField::Bind(ID3D12GraphicsCommandList *cmd, SlotKey &key, bool on)
{
    key.res[kSlot] = nullptr;
    key.fmt[kSlot] = kFieldFormat; // a null view reads as no support
    key.res[kNowSlot] = nullptr;
    key.fmt[kNowSlot] = kFieldFormat;
    if (!on || !built_ || count_ == 0) return;
    ID3D12Resource *field = Filled(0);
    Barrier(cmd, field, FilledState(0), kReadable);
    key.res[kSlot] = field;
    Level &coarsest = levels_[count_ - 1];
    Barrier(cmd, coarsest.now, coarsest.nowState, kReadable);
    key.res[kNowSlot] = coarsest.now;
}

} // namespace ofps::core::temporal
