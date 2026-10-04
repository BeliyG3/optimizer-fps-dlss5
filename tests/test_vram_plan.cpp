// Which optional textures a configuration gets (VRAM step 1): the warp's texture plan, the target
// judgement that decides the copy intermediates, and the base/transfer rule the plan relies on.
#include "core/frame/warp_recorder.h"
#include "core/temporal/machine_role.h"
#include "core/warp/compute.h"
#include "core/warp/texture_plan.h"

#include <cstdio>
#include <cstdlib>

using namespace ofps::core::warp;

namespace {
int failures = 0;

void Check(bool value, const char *name)
{
    if (value) return;
    std::fprintf(stderr, "FAIL %s\n", name);
    ++failures;
}

TextureNeeds Needs(PackPath path, bool temporal, bool transferReplacesBase)
{
    TextureNeeds n;
    n.path = path;
    n.temporal = temporal;
    n.warpBase = true;
    n.transferReplacesBase = transferReplacesBase;
    return n;
}

void WarpPlans()
{
    // Default: compute, temporal off, detail transfer (ColorFilter 2), direct-UAV output.
    auto plan = PlanTextures(Needs(PackPath::Compute, false, true));
    Check(plan.packSlots == 4 && !plan.copyIntermediates && !plan.unpackTarget && !plan.unpackBase,
          "default compute set: four slots, no intermediates, no native targets");
    plan = PlanTextures(Needs(PackPath::Compute, false, false));
    Check(!plan.unpackTarget && !plan.unpackBase, "ColorFilter 1 without a temporal mode: no native targets");

    plan = PlanTextures(Needs(PackPath::Compute, true, true));
    Check(plan.unpackTarget && !plan.unpackBase, "compute temporal with the transfer: target, no base");
    plan = PlanTextures(Needs(PackPath::Compute, true, false));
    Check(plan.unpackTarget && plan.unpackBase, "compute temporal without the transfer: target and base");
    auto noBase = Needs(PackPath::Compute, true, false);
    noBase.warpBase = false;
    Check(!PlanTextures(noBase).unpackBase, "warpBase off: no base");

    auto background = Needs(PackPath::Compute, true, true);
    background.background = true;
    Check(PlanTextures(background).packSlots == 5, "background-capable set: the fifth slot");

    auto copy = Needs(PackPath::Compute, false, true);
    copy.copyFallback = true;
    Check(PlanTextures(copy).copyIntermediates, "a non-UAV host output: compute intermediates");

    plan = PlanTextures(Needs(PackPath::Pixel, false, false));
    Check(plan.packSlots == 4 && plan.unpackTarget && !plan.unpackBase,
          "pixel without a temporal mode: the target the draw writes, no base");
    auto pixelCopy = Needs(PackPath::Pixel, true, false);
    pixelCopy.copyFallback = true;
    plan = PlanTextures(pixelCopy);
    Check(plan.unpackTarget && plan.unpackBase && !plan.copyIntermediates,
          "pixel temporal: target and base, never compute intermediates");

    Check(PlanTextures(Needs(PackPath::None, true, false)) == TexturePlan{}, "no warp path: nothing");
}

D3D12_RESOURCE_DESC Texture(DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags)
{
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = 64;
    d.Height = 32;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = format;
    d.SampleDesc.Count = 1;
    d.Flags = flags;
    return d;
}

bool Copy(const D3D12_RESOURCE_DESC &d, DXGI_FORMAT view, unsigned subresource)
{
    return NeedsCopyFallback(TargetUnpackSupport(d, view, view, subresource, true, true));
}

void CopyFallbackJudgement()
{
    constexpr auto kF16 = DXGI_FORMAT_R16G16B16A16_FLOAT;
    constexpr auto kUav = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    Check(!Copy(Texture(kF16, kUav), kF16, 0), "UAV output: direct write");
    Check(!Copy(Texture(DXGI_FORMAT_R16G16B16A16_TYPELESS, kUav), kF16, 0), "typeless UAV output, typed view: direct");
    Check(Copy(Texture(kF16, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET), kF16, 0), "no UAV flag: copy");
    Check(Copy(Texture(kF16, kUav), kF16, 1), "subresource 1: copy");
    auto array = Texture(kF16, kUav);
    array.DepthOrArraySize = 2;
    Check(Copy(array, kF16, 0), "array output: copy");
    auto msaa = Texture(kF16, kUav);
    msaa.SampleDesc.Count = 4;
    Check(Copy(msaa, kF16, 0), "multisampled output: copy");
    Check(!Copy(Texture(DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE), kF16, 0),
          "incompatible view: neither path, so no intermediate");
    const auto noStore = TargetUnpackSupport(Texture(kF16, kUav), kF16, kF16, 0, false, true);
    Check(NeedsCopyFallback(noStore), "a view without typed stores: copy");
}

void BaseTransferRule()
{
    ofps::sdk::LayoutV2 layout{};
    layout.colorFilter = ofps::sdk::ColorFilter::DetailTransfer;
    using ofps::core::TransferReplacesBase;
    constexpr auto kF16 = DXGI_FORMAT_R16G16B16A16_FLOAT;
    Check(TransferReplacesBase(layout, PackPath::Compute, kF16, kF16), "compute transfer replaces the base");
    Check(!TransferReplacesBase(layout, PackPath::Pixel, kF16, kF16), "pixel keeps its base");
    Check(!TransferReplacesBase(layout, PackPath::Compute, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, kF16),
          "a colour-space mismatch keeps the base");
    layout.colorFilter = ofps::sdk::ColorFilter::AdaptiveFourTap;
    Check(!TransferReplacesBase(layout, PackPath::Compute, kF16, kF16), "ColorFilter 1 keeps the base");
}
void MachineRoles()
{
    using namespace ofps::core::temporal;
    const auto sync = TexturesFor(MachineRole::Synchronous);
    Check(!sync.pendingChains && !sync.kickExpectation && !sync.residualMix,
          "synchronous machine: no background chains, kick expectation or residual mix");
    Check(sync.residualOld && sync.history && sync.previousResidual,
          "synchronous machine: phase-in, history and the blend source stay");
    const auto background = TexturesFor(MachineRole::Background);
    Check(background.pendingChains && background.kickExpectation && background.residualMix && background.residualOld &&
              background.history && background.previousResidual,
          "background machine: everything");
    const auto hidden = TexturesFor(MachineRole::Hidden);
    Check(!hidden.pendingChains && !hidden.kickExpectation && !hidden.residualMix && !hidden.residualOld &&
              !hidden.history && !hidden.previousResidual,
          "hidden spread machine: one residual, no history, no phase-in, no background chains");
}
} // namespace

int main()
{
    WarpPlans();
    CopyFallbackJudgement();
    BaseTransferRule();
    MachineRoles();
    if (failures) return EXIT_FAILURE;
    std::puts("vram plan: pass");
    return EXIT_SUCCESS;
}
